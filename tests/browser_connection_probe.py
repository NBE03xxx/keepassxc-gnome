#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the synthetic Native Messaging round trip in an isolated Chrome profile."""

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HOST_NAME = "org.keepassxc.keepassxc_browser"
STORE_EXTENSION = "oboonakemofpalcgghocfoadofidjkkk"
EXTENSION_VERSION = "1.10.4.1"
MAX_FRAME = 64 * 1024
SYNTHETIC_HASH = "97ac79e04c187bc8530cacfc57afa166e18a77806b511bf8b5a0f2746ce287f05"


def check_peer(sock):
    credentials = sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("iII"))
    _, uid, _ = struct.unpack("iII", credentials)
    if uid != os.getuid():
        raise PermissionError("Unexpected socket peer UID")


def read_frame(stream):
    header = stream.read(4)
    if not header:
        return None
    size, = struct.unpack("=I", header)
    if not 0 < size <= MAX_FRAME:
        raise ValueError("Invalid Native Messaging frame length")
    body = stream.read(size)
    if len(body) != size:
        raise EOFError("Truncated Native Messaging frame")
    return header + body


def run_host(socket_path, allowed_origin, origin):
    if origin != allowed_origin:
        raise PermissionError("Unexpected extension origin")
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(8)
        peer.connect(socket_path)
        check_peer(peer)
        with peer.makefile("rb") as replies:
            while frame := read_frame(sys.stdin.buffer):
                peer.sendall(frame)
                response = read_frame(replies)
                if response is None:
                    raise EOFError("Responder closed without replying")
                sys.stdout.buffer.write(response)
                sys.stdout.buffer.flush()


@contextmanager
def run_responder(socket_path, executable):
    stop = threading.Event()
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(os.fspath(socket_path))
        listener.listen(4)
        listener.settimeout(0.2)

        def serve():
            children = []
            try:
                while not stop.is_set():
                    children = [child for child in children if child.poll() is None]
                    try:
                        connection, _ = listener.accept()
                    except TimeoutError:
                        continue
                    with connection:
                        try:
                            check_peer(connection)
                        except PermissionError:
                            continue
                        if len(children) < 4:
                            children.append(subprocess.Popen([executable], stdin=connection, stdout=connection))
            finally:
                for child in children:
                    if child.poll() is None:
                        child.kill()
                    child.wait()

        worker = threading.Thread(target=serve)
        worker.start()
        try:
            yield
        finally:
            stop.set()
            worker.join()


class DevTools:
    def __init__(self, write_fd, read_fd):
        self.write_fd = write_fd
        self.read_fd = read_fd
        self.buffer = bytearray()
        self.id = 0

    def call(self, method, params=None, session=None):
        self.id += 1
        request = {"id": self.id, "method": method, "params": params or {}}
        if session:
            request["sessionId"] = session
        os.write(self.write_fd, json.dumps(request).encode() + b"\0")
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            while (end := self.buffer.find(b"\0")) != -1:
                message = json.loads(self.buffer[:end])
                del self.buffer[:end + 1]
                if message.get("id") == self.id:
                    if "error" in message:
                        raise RuntimeError(message["error"])
                    return message["result"]
            if select.select([self.read_fd], [], [], max(0, deadline - time.monotonic()))[0]:
                chunk = os.read(self.read_fd, 65536)
                if not chunk:
                    raise EOFError("Chrome closed the DevTools pipe")
                self.buffer.extend(chunk)
        raise TimeoutError(f"Chrome did not complete {method}")

    def evaluate(self, session, expression):
        result = self.call("Runtime.evaluate", {
            "expression": expression, "awaitPromise": True, "returnByValue": True
        }, session)
        if "exceptionDetails" in result:
            raise RuntimeError(result["result"].get("description", "Extension evaluation failed"))
        return result["result"].get("value")


@contextmanager
def run_chrome(chrome, extension, profile, environment, test_flags=()):
    browser_read, test_write = os.pipe()
    test_read, browser_write = os.pipe()
    # Chrome uses FD 3 for incoming commands and FD 4 for responses.
    descriptors = [fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 10) for fd in (browser_read, browser_write)]
    os.close(browser_read)
    os.close(browser_write)
    log_path = profile.parent / "chrome.log"
    extension_flags = [] if extension is None else [
        f"--load-extension={extension}", "--disable-background-networking"
    ]
    with log_path.open("w+") as log:
        try:
            browser = subprocess.Popen([
                "/bin/bash", "-c",
                'incoming=$1; outgoing=$2; shift 2; '
                'exec 3<&"$incoming" 4>&"$outgoing"; exec {incoming}<&- {outgoing}>&-; exec "$@"',
                "bash",
                *map(str, descriptors), os.fspath(chrome), "--headless",
                f"--user-data-dir={profile}", "--remote-debugging-pipe", "--no-first-run",
                "--disable-sync", "--no-default-browser-check", "--password-store=basic",
                *extension_flags, *test_flags, "about:blank",
            ], pass_fds=descriptors, env=environment, stdout=log, stderr=log)
        finally:
            for fd in descriptors:
                os.close(fd)
        try:
            yield DevTools(test_write, test_read)
        except Exception:
            log.seek(0)
            print(log.read()[-6000:], file=sys.stderr)
            raise
        finally:
            os.close(test_write)
            os.close(test_read)
            try:
                browser.wait(timeout=4)
            except subprocess.TimeoutExpired:
                browser.terminate()
                try:
                    browser.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    browser.kill()
                    browser.wait()


def check_extension(devtools, origin, worker, timeout):
    worker_url = origin + worker
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        targets = devtools.call("Target.getTargets")["targetInfos"]
        target = next((target for target in targets if target["url"] == worker_url), None)
        if target:
            break
        time.sleep(0.15)
    else:
        raise TimeoutError("Official extension service worker did not start")
    session = devtools.call("Target.attachToTarget", {"targetId": target["targetId"], "flatten": True})["sessionId"]
    result = devtools.evaluate(session, """
        (async () => {
            const deadline = Date.now() + 5000;
            while (!keepass.databaseHash && Date.now() < deadline) {
                await new Promise(resolve => setTimeout(resolve, 100));
            }
            return {
                version: chrome.runtime.getManifest().version,
                connected: Boolean(keepass.isConnected && keepass.serverPublicKey),
                hash: keepass.databaseHash,
                associated: keepass.associated.value,
            };
        })()
    """)
    expected = {"version": EXTENSION_VERSION, "connected": True, "hash": SYNTHETIC_HASH, "associated": False}
    if result != expected:
        raise RuntimeError(f"Unexpected extension state: {result}")
    print("PASS: official extension verified key exchange and decrypted the synthetic response")
    rejected = devtools.evaluate(session, """
        (async () => {
            const rejected = [];
            for (const action of ['associate', 'test-associate', 'get-logins']) {
                const [nonce] = keepassClient.getNonces();
                const message = keepassClient.encrypt({action}, nonce);
                const request = keepassClient.buildRequest(action, message, nonce, keepass.clientID);
                const reply = await keepassClient.sendNativeMessage(request, true, 1500);
                rejected.push(reply?.action === action && reply?.errorCode === '12' && !reply?.message);
            }
            return rejected;
        })()
    """)
    if rejected != [True, True, True]:
        raise RuntimeError(f"Unexpected association and credential responses: {rejected}")
    print("PASS: association, association testing and credential requests were rejected")


def run_probe(chrome, extension, responder):
    extension_id = STORE_EXTENSION
    worker = "background/background_service.js"
    if extension is not None:
        source_manifest = json.loads((extension / "manifest.json").read_text())
        if source_manifest["version"] != EXTENSION_VERSION or "key" in source_manifest:
            raise ValueError(f"Use the unmodified official Chromium ZIP for version {EXTENSION_VERSION}")
        worker = source_manifest["background"]["service_worker"]
        # Chromium derives the ID of an unpacked, unkeyed extension from its absolute path.
        digest = hashlib.sha256(os.fsencode(extension)).hexdigest()[:32]
        extension_id = digest.translate(str.maketrans("0123456789abcdef", "abcdefghijklmnop"))
    origin = f"chrome-extension://{extension_id}/"
    with tempfile.TemporaryDirectory(prefix="kpxc-browser-") as directory:
        root = Path(directory)
        profile = root / "profile"
        socket_path = root / "probe.sock"
        host = root / "keepassxc-gnome-browser-probe-host"
        command = [sys.executable, Path(__file__).resolve(), "--host", socket_path, origin]
        host.write_text("#!/bin/sh\nexec " + shlex.join(map(os.fspath, command)) + ' "$@"\n')
        host.chmod(0o700)
        registration = profile / "NativeMessagingHosts"
        registration.mkdir(parents=True)
        (registration / f"{HOST_NAME}.json").write_text(json.dumps({
            "name": HOST_NAME,
            "description": "KeePassXC GNOME synthetic browser connection test",
            "path": str(host),
            "type": "stdio",
            "allowed_origins": [origin],
        }))
        environment = os.environ.copy()
        for name, path in {
            "HOME": root,
            "XDG_CONFIG_HOME": root / "config",
            "XDG_CACHE_HOME": root / "cache",
            "XDG_STATE_HOME": root / "state",
            "XDG_RUNTIME_DIR": root,
        }.items():
            environment[name] = str(path)
        with run_responder(socket_path, responder), run_chrome(chrome, extension, profile, environment) as devtools:
            check_extension(devtools, origin, worker, 55 if extension is None else 8)


def main():
    if sys.argv[1:2] == ["--host"]:
        run_host(*sys.argv[2:])
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("chrome", type=Path, help="Chrome executable")
    parser.add_argument("extension", type=Path, nargs="?", help="unpacked official Chromium extension; "
                        "omit for policy-installed Chrome Web Store extension")
    parser.add_argument("responder", type=Path, help="gnome_browser_connection_probe executable")
    args = parser.parse_args()
    if os.getuid() == 0:
        parser.error("Run the probe as an unprivileged user in a dedicated VM")
    extension = args.extension.resolve(strict=True) if args.extension else None
    run_probe(args.chrome.resolve(strict=True), extension, args.responder.resolve(strict=True))


if __name__ == "__main__":
    main()
