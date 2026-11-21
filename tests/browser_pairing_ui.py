#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the packaged GTK UI and the unmodified store extension in a private VM session."""
from contextlib import contextmanager
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import select
import socket as sockets
import ssl
import struct
import subprocess
import sys
import time
import threading

import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, Gio, GLib

from browser_connection_probe import EXTENSION_VERSION, HOST_NAME, STORE_EXTENSION, run_chrome

LOGIN_PATH = "/org/freedesktop/login1"
LOGIN_NAME = "org.freedesktop.login1"
LOGIN_MANAGER = LOGIN_NAME + ".Manager"


def serve_private_logind():
    assert os.environ["DBUS_SYSTEM_BUS_ADDRESS"] == os.environ["DBUS_SESSION_BUS_ADDRESS"]
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    info = Gio.DBusNodeInfo.new_for_xml(f"""
        <node><interface name="{LOGIN_MANAGER}">
          <property name="PreparingForSleep" type="b" access="read"/>
          <method name="Inhibit">
            <arg type="s" direction="in"/><arg type="s" direction="in"/>
            <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="h" direction="out"/>
          </method>
          <signal name="PrepareForSleep"><arg type="b"/></signal>
        </interface></node>
    """)
    loop = GLib.MainLoop()
    sleeping = False

    def inhibit(connection, sender, path, interface, method, parameters, invocation):
        fd = os.open(os.devnull, os.O_RDONLY)
        fds = Gio.UnixFDList.new()
        try:
            handle = fds.append(fd)
            invocation.return_value_with_unix_fd_list(GLib.Variant("(h)", (handle,)), fds)
        finally:
            os.close(fd)

    def signal(_source, _condition):
        nonlocal sleeping
        value = sys.stdin.readline().strip()
        if value not in ("sleep", "wake"):
            loop.quit()
            return GLib.SOURCE_REMOVE
        sleeping = value == "sleep"
        bus.emit_signal(None, LOGIN_PATH, LOGIN_MANAGER, "PrepareForSleep", GLib.Variant("(b)", (sleeping,)))
        return GLib.SOURCE_CONTINUE

    bus.register_object(LOGIN_PATH, info.interfaces[0], inhibit, lambda *_: GLib.Variant("b", sleeping), None)
    reply = bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                          GLib.Variant("(su)", (LOGIN_NAME, 4)), GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, 5000, None)
    assert reply.unpack() == (1,)
    GLib.io_add_watch(sys.stdin, GLib.PRIORITY_DEFAULT, GLib.IO_IN | GLib.IO_HUP, signal)
    print("ready", flush=True)
    loop.run()


@contextmanager
def private_logind():
    helper = subprocess.Popen([sys.executable, __file__, "--private-logind"], text=True,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        assert select.select([helper.stdout], [], [], 6)[0] and helper.stdout.readline().strip() == "ready"

        def set_sleep(value):
            helper.stdin.write("sleep\n" if value else "wake\n")
            helper.stdin.flush()

        yield set_sleep
    finally:
        helper.stdin.close()
        helper.stdout.close()
        try:
            helper.wait(timeout=5)
        except subprocess.TimeoutExpired:
            helper.kill()
            helper.wait()


@contextmanager
def https_forms(root):
    primary, alternate = "login.kpxc.test", "other.kpxc.test"
    certificate, key = root / "form.crt", root / "form.key"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", key, "-out", certificate, "-days", "1", "-subj", f"/CN={primary}",
                    "-addext", f"subjectAltName=DNS:{primary},DNS:{alternate}"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    public_key = subprocess.check_output(["openssl", "x509", "-in", certificate, "-pubkey", "-noout"])
    spki = subprocess.check_output(["openssl", "pkey", "-pubin", "-outform", "DER"], input=public_key)

    class Forms(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path == "/iframe":
                body = f'<iframe src="{self.server.origin}/login"></iframe>'
            else:
                action = self.server.alternate + "/submit" if self.path == "/cross" else "/submit"
                body = f'''<form method="post" action="{action}">
                    <label>Username <input name="login" autocomplete="username" id="user"></label>
                    <label>Password <input name="password" type="password" autocomplete="current-password" id="password"></label>
                    <button type="submit">Sign in</button>
                </form>'''
            data = ("<!doctype html><meta charset=utf-8><title>Synthetic login</title>" + body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            self.server.submitted = True
            self.send_response(204)
            self.end_headers()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Forms)
    port = server.server_port
    server.origin = f"https://{primary}:{port}"
    server.alternate = f"https://{alternate}:{port}"
    server.submitted = False
    server.flags = [f"--ignore-certificate-errors-spki-list={base64.b64encode(hashlib.sha256(spki).digest()).decode()}",
                    f"--host-resolver-rules=MAP {primary} 127.0.0.1, MAP {alternate} 127.0.0.1", "--no-proxy-server"]
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(certificate, key)
    server.socket = tls.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def wait(test, label, seconds=12):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            result = test()
            if result:
                return result
        except (GLib.Error, FileNotFoundError):
            pass
        time.sleep(0.08)
    raise AssertionError(label)


def walk(node, depth=0):
    if not node or depth > 40:
        return
    node.clear_cache()
    yield node
    for index in range(node.get_child_count()):
        yield from walk(node.get_child_at_index(index), depth + 1)


class UI:
    def __init__(self, pid, japanese):
        self.pid = pid
        self.japanese = japanese

    def nodes(self, dialog=False):
        app = next((a for a in Atspi.get_desktop(0) if a and a.get_process_id() == self.pid), None)
        if dialog:
            app = next((n for n in walk(app) if n.get_role_name() in ("alert", "dialog")), None)
        return list(walk(app))

    def text(self, en, ja):
        return ja if self.japanese else en

    def action(self, en, ja, dialog=False):
        label = self.text(en, ja)

        def invoke():
            for n in self.nodes(dialog):
                if n.get_name().strip() != label or not n.get_state_set().contains(Atspi.StateType.SENSITIVE):
                    continue
                for child in walk(n):
                    action = child.get_action_iface()
                    if action and action.get_n_actions() and action.do_action(0):
                        return True
            return False
        try:
            wait(invoke, "action " + label, 8)
        except AssertionError:
            print([(n.get_role_name(), n.get_name()) for n in self.nodes(dialog)], file=sys.stderr)
            raise

    def settings(self):
        self.action("Database menu", "データベースメニュー")
        self.action("Browser integration", "ブラウザー連携")

    def unlock(self):
        field = wait(lambda: next((n for n in self.nodes(True) if n.get_role_name() == "password text"), None),
                     "password prompt")
        assert field.get_editable_text_iface().set_text_contents("phase2-demo")
        self.action("Unlock", "ロック解除", True)

    def pending(self):
        title = self.text("Allow browser connection?", "ブラウザーからの接続を許可しますか？")
        return any(n.get_name() == title for n in self.nodes(True))

    def login_pending(self):
        title = self.text("Allow login for this site?", "このサイトへのログイン情報の入力を許可しますか？")
        return any(n.get_name() == title for n in self.nodes(True))

    def approve(self, filename):
        wait(self.pending, "explicit browser approval")
        dialog = self.nodes(True)
        assert any(filename in n.get_name() for n in dialog), "approved database named in dialog"
        field = next(n.get_editable_text_iface() for n in dialog if n.get_editable_text_iface())
        assert field.set_text_contents("bad/path")
        wait(lambda: any(n.get_name() == self.text("Allow connection", "接続を許可")
                         and "button" in n.get_role_name()
                         and not n.get_state_set().contains(Atspi.StateType.SENSITIVE) for n in self.nodes(True)),
             "invalid connection name keeps the approval button disabled")
        assert field.set_text_contents("Chrome 試験")
        self.action("Allow connection", "接続を許可", True)


def activate_worker(devtools, origin):
    url = origin + "background/background_service.js"
    target = wait(lambda: next((item for item in devtools.call("Target.getTargets")["targetInfos"]
                                if item["url"] == url), None), "installed Web Store service worker", 55)
    return devtools.call("Target.attachToTarget", {"targetId": target["targetId"], "flatten": True})["sessionId"]


def exercise_fill(devtools, session, ui, forms, sleep, reconnect):
    evaluate = lambda script: devtools.evaluate(session, script)
    assert evaluate("""(async () => {
        await browser.storage.local.set({settings: {...page.settings,
            autoRetrieveCredentials: false, autoSubmit: true, autoFillSingleEntry: false}});
        const settings = await page.initSettings();
        return settings.autoSubmit && !settings.autoRetrieveCredentials;
    })()""")
    tab = next(target for target in devtools.call("Target.getTargets")["targetInfos"] if target["url"] == "about:blank")
    devtools.call("Target.activateTarget", {"targetId": tab["targetId"]})
    tab_session = devtools.call("Target.attachToTarget", {"targetId": tab["targetId"], "flatten": True})["sessionId"]
    page = lambda script: devtools.evaluate(tab_session, script)

    def navigate(url, iframe=False):
        assert "errorText" not in devtools.call("Page.navigate", {"url": url}, tab_session)
        wait(lambda: page(f"location.href === {json.dumps(url)} && document.readyState === 'complete'"), "form loaded")
        if not iframe:
            assert page("document.querySelector('#user').focus(); true")

    def start_fill(iframe=False):
        assert evaluate(f"""(() => {{
            globalThis.kpxcFillDone = false;
            (async () => {{
                const [tab] = await chrome.tabs.query({{active:true}});
                const message = {{action: 'fill_username_password'}};
                const frame = {str(iframe).lower()} && (await chrome.webNavigation.getAllFrames({{tabId: tab.id}}))
                    .find(frame => frame.frameId > 0);
                await chrome.tabs.sendMessage(tab.id, message, {{frameId: frame ? frame.frameId : 0}});
            }})().then(() => globalThis.kpxcFillDone = true, error => globalThis.kpxcFillDone = String(error));
            return true;
        }})()""")

    def start_login(case):
        navigate(forms.origin + "/login?" + case)
        start_fill()

        def prompt():
            if ui.login_pending():
                return True
            if evaluate("globalThis.kpxcFillDone") is not False:
                # The first request can precede the extension's field scan.
                assert page("!document.querySelector('#password').value")
                start_fill()
            return False

        wait(prompt, "site login request reaches GTK: " + case)
        assert page("!document.querySelector('#user').value && !document.querySelector('#password').value")

    def reject_request(url, iframe=False):
        navigate(url, iframe)
        start_fill(iframe)
        end = time.monotonic() + 1.5
        while time.monotonic() < end:
            assert not ui.login_pending(), "untrusted frame or action must not request approval"
            time.sleep(.08)
        outcome = evaluate("globalThis.kpxcFillDone")
        assert outcome is True, f"extension fill command finished: {outcome!r}"
        if not iframe:
            assert page("!document.querySelector('#password').value")

    reject_request(forms.origin + "/cross")
    reject_request(forms.alternate + "/iframe", True)
    start_login("cancel")
    assert any(forms.origin in n.get_name() for n in ui.nodes(True)), "dialog displays exact HTTPS origin"
    ui.action("Cancel", "キャンセル", True)
    try:
        wait(lambda: not ui.login_pending(), "login approval closed")
        wait(lambda: evaluate("globalThis.kpxcFillDone") is not False, "cancelled fill completed")
    except AssertionError:
        print("Fill completion:", evaluate("globalThis.kpxcFillDone"), file=sys.stderr)
        print("Visible dialog:", [(n.get_role_name(), n.get_name()) for n in ui.nodes(True)], file=sys.stderr)
        raise
    assert page("!document.querySelector('#password').value") and not forms.submitted

    start_login("fill")
    ui.action("Allow Primary synthetic login — Synthetic logins",
              "Primary synthetic login — Synthetic logins の入力を許可", True)
    wait(lambda: page("document.querySelector('#user').value === 'synthetic-login-user' && "
                      "document.querySelector('#password').value === 'synthetic-login-password'"),
         "unmodified extension inserts the selected credential into the HTML form")
    end = time.monotonic() + 0.75
    while time.monotonic() < end:
        assert not forms.submitted, "response suppresses auto-submit even with extension auto-submit set"
        time.sleep(.05)
    print("PASS: official extension filled the selected HTTPS form without submitting it", flush=True)
    ui.action("Lock", "ロック")
    wait(lambda: evaluate("""(async () => {
        const [tab] = await chrome.tabs.query({active:true});
        return !keepass.isConnected && !tabs.getTabFromId(tab?.id)?.credentials?.length;
    })()"""), "browser clears its cached credentials on lock")

    ui.action("Unlock database", "データベースのロック解除")
    ui.unlock()
    wait(lambda: evaluate(reconnect) and evaluate("keepass.isAssociated()"), "browser reauthorized after lock")
    navigate(forms.origin + "/login?lock")
    # The store extension's background handler uses the same request as its
    # content script. Exercise an in-flight request without carrying a
    # cancelled content-script Promise into the next form-fill case.
    evaluate("""(() => {
        chrome.tabs.query({active:true}).then(([tab]) => keepass.retrieveCredentials(tab, [
            tab.url, new URL('/submit', tab.url).href
        ]));
        return true;
    })()""")
    wait(ui.login_pending, "site login request waiting at desktop lock")
    sleep(True)
    wait(lambda: not ui.login_pending() and not evaluate("keepass.isConnected"), "lock cancels credential release")
    assert page("!document.querySelector('#password').value")


def main():
    application, fixture_writer, chrome, root = (Path(arg).resolve() for arg in sys.argv[1:])
    config = Path(os.environ["XDG_CONFIG_HOME"]).resolve()
    runtime = Path(os.environ["XDG_RUNTIME_DIR"]).resolve()
    assert root in config.parents and root in runtime.parents
    assert os.environ["DBUS_SYSTEM_BUS_ADDRESS"] == os.environ["DBUS_SESSION_BUS_ADDRESS"]
    with https_forms(root) as forms:
        run_workflow(application, fixture_writer, chrome, root, config, runtime, forms)


def run_workflow(application, fixture_writer, chrome, root, config, runtime, forms):
    profile = config / "google-chrome"
    homes = profile / "NativeMessagingHosts"
    manifest = homes / (HOST_NAME + ".json")
    socket = runtime / "keepassxc-gnome-browser/browser.sock"
    fixture = root / "Synthetic browser DB.kdbx"
    subprocess.run([fixture_writer, "--write-login-fixture", fixture, forms.origin], check=True)
    original = hashlib.sha256(fixture.read_bytes()).hexdigest()

    with private_logind() as sleep, (root / "app.log").open("w+") as log:
        process = subprocess.Popen([application, fixture], stdout=log, stderr=log)
        try:
            ui = UI(process.pid, os.environ.get("LANGUAGE") == "ja")
            ui.unlock()
            wait(lambda: not ui.nodes(True), "initial unlock dialog finished")
            # Initial state is inactive. Foreign registrations are preserved.
            assert not manifest.exists() and not socket.exists()
            homes.mkdir(parents=True)
            manifest.write_text('{"external-test-owner":true}\n')
            foreign = manifest.read_bytes()
            ui.settings()
            wait(lambda: any(ui.text("Its registration has been kept", "既存の設定はそのまま") in n.get_name()
                             for n in ui.nodes(True)), "foreign manifest conflict visible")
            ui.action("Close", "閉じる", True)
            wait(lambda: not ui.nodes(True), "conflict dialog finished closing")
            assert manifest.read_bytes() == foreign and not socket.exists()
            manifest.unlink()  # remove only the fixture created by this test
            ui.settings()
            ui.action("Enable for Chrome", "Chrome との連携を有効にする", True)
            wait(lambda: manifest.exists() and socket.is_socket(), "actual product registration and socket")
            registration = json.loads(manifest.read_text())
            origin = f"chrome-extension://{STORE_EXTENSION}/"
            assert registration["allowed_origins"] == [origin]
            assert registration["path"] == str(application.parent / "keepassxc-gnome-browser-host")
            invalid_origin = subprocess.run([registration["path"], "chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"],
                                            input=b"", capture_output=True, timeout=5)
            assert invalid_origin.returncode != 0 and not invalid_origin.stdout
            for malformed in (struct.pack("=I", 65537), struct.pack("=I", 1) + b"\xff"):
                with sockets.socket(sockets.AF_UNIX) as peer:
                    peer.settimeout(3)
                    peer.connect(os.fspath(socket))
                    peer.sendall(malformed)
                    assert not peer.recv(1), "malformed frame closes only that peer"
            # The app registers the standard path in config. Use a separate
            # Chrome config base so Chrome permits the test's local CDP pipe.
            chrome_env = dict(os.environ, XDG_CONFIG_HOME=str(root / "chrome-cdp-config"))
            with run_chrome(chrome, None, profile, chrome_env, forms.flags) as devtools:
                session = activate_worker(devtools, origin)
                evaluate = lambda expression: devtools.evaluate(session, expression)
                wait(lambda: evaluate("typeof chrome !== 'undefined' && typeof keepass !== 'undefined' && !!page?.settings"),
                     "extension scripts initialized")
                assert evaluate("chrome.runtime.getManifest().version") == EXTENSION_VERSION
                reconnect = """(async () => {
                    const [tab] = await chrome.tabs.query({active:true});
                    return Boolean(await keepass.reconnect(tab));
                })()"""
                wait(lambda: evaluate(reconnect), "Chrome native host connects")
                assert len(evaluate("keepass.databaseHash")) == 64, "extension decrypts the real DB hash"
                begin = """(() => {
                    globalThis.kpxcPairResult = null;
                    chrome.tabs.query({active:true}).then(([tab]) => keepass.associate(tab)).then(
                        () => { globalThis.kpxcPairResult = keepass.isAssociated() ? 'paired' : 'denied'; },
                        error => { globalThis.kpxcPairResult = String(error); });
                    return true;
                })()"""

                def pairing_prompt(label):
                    assert evaluate(begin)

                    def visible():
                        if ui.pending():
                            return True
                        if evaluate("globalThis.kpxcPairResult === 'denied'"):
                            # GTK finishing a previous close can briefly reject
                            # a new dialog. A fresh request must still work.
                            assert evaluate(begin)
                        return False

                    wait(visible, label)

                pairing_prompt("pairing to be approved reaches GTK")
                ui.approve(fixture.name)
                wait(lambda: evaluate("globalThis.kpxcPairResult === 'paired'"), "official extension accepted approved association")
                assert hashlib.sha256(fixture.read_bytes()).hexdigest() == original, "approval checkpoints before saving"
                assert evaluate("keepass.isAssociated()")
                rejected = evaluate("""(async () => {
                    const nonce = keepassClient.getNonce();
                    const reply = await keepassClient.sendNativeMessage({ action: 'get-totp', nonce }, true, 1500);
                    return reply?.errorCode;
                })()""")
                assert rejected == "12", "TOTP stays outside this fill flow"
                ui.action("Save", "保存")
                wait(lambda: any(n.get_name() == ui.text("Database saved.", "データベースを保存しました。")
                                 for n in ui.nodes()), "normal save finishes")
                assert hashlib.sha256(fixture.read_bytes()).hexdigest() != original

                ui.action("Lock", "ロック")
                wait(lambda: not evaluate("keepass.isAssociated()"), "lock disconnects Chrome and clears associated state")
                locked = evaluate(f"""(async () => {{
                    const reply = await chrome.runtime.sendNativeMessage({json.dumps(HOST_NAME)}, {{action: 'get-databasehash'}});
                    return reply.errorCode;
                }})()""")
                assert locked == "1" and not ui.pending(), "locked database refuses requests without prompting"
                ui.action("Unlock database", "データベースのロック解除")
                ui.unlock()
                wait(lambda: evaluate(reconnect) and evaluate("keepass.isAssociated()"), "official extension reauthorizes after reopening saved DB")

                process.terminate()
                process.wait(timeout=5)
                process = subprocess.Popen([application, fixture], stdout=log, stderr=log)
                ui = UI(process.pid, ui.japanese)
                ui.unlock()
                wait(lambda: evaluate(reconnect) and evaluate("keepass.isAssociated()"), "saved manifest and pairing work after app restart")
                ui.settings()
                ui.action("Disable for Chrome", "Chrome との連携を無効にする", True)
                wait(lambda: not manifest.exists() and not socket.exists(), "disable removes own registration and socket")
                wait(lambda: not evaluate("keepass.isAssociated()"), "disable disconnects extension")
                wait(lambda: not ui.nodes(True), "disable dialog finished closing")
                ui.settings()
                ui.action("Enable for Chrome", "Chrome との連携を有効にする", True)
                wait(lambda: evaluate(reconnect) and evaluate("keepass.isAssociated()"), "re-enabled browser available")
                exercise_fill(devtools, session, ui, forms, sleep, reconnect)
        except Exception:
            log.seek(0)
            print(log.read()[-6000:], file=sys.stderr)
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    print("PASS: packaged GTK + Chrome Web Store extension, pairing and selected HTTPS form fill, "
          "cross-origin action and iframe refusal, cancellation, lock and no auto-submit")


if __name__ == "__main__":
    if sys.argv[1:] == ["--private-logind"]:
        serve_private_logind()
    else:
        main()
