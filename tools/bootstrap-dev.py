#!/usr/bin/env python3
"""Extract the previously approved Ubuntu development packages into a local prefix."""

import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "development"
PACKAGES = (
    "cmake", "qt6-base-dev", "qt6-svg-dev", "qt6-tools-dev", "libbotan-3-dev",
    "libminizip-dev", "libpcsclite-dev", "libusb-1.0-0-dev", "libkeyutils-dev",
    "libqrencode-dev", "libgtk-4-dev", "libadwaita-1-dev", "libei-dev",
)


def download(item):
    path = CACHE / "packages" / item["file"]
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == item["sha256"]:
        return path
    temporary = path.with_suffix(".download")
    with urllib.request.urlopen(item["url"], timeout=90) as response:
        with temporary.open("wb") as output:
            while data := response.read(1024 * 1024):
                output.write(data)
    if hashlib.sha256(temporary.read_bytes()).hexdigest() != item["sha256"]:
        temporary.unlink()
        raise RuntimeError(f"Package checksum mismatch for {item['file']}")
    temporary.replace(path)
    return path


def main():
    (CACHE / "packages").mkdir(parents=True, exist_ok=True)
    prefix = CACHE / "sysroot"
    manifest_path = CACHE / "packages.json"
    # An incremental APT plan can omit packages that are now installed on the
    # host. Their previous copies still exist in the SDK and the packager needs
    # their verified shlibs metadata.
    previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else []
    packages = {item["file"].split("_", 1)[0]: item for item in previous}
    plan = subprocess.check_output([
        "apt-get", "--print-uris", "--yes", "--download-only", "--no-install-recommends",
        "-o", "Acquire::ForceHash=SHA256", "install", *PACKAGES,
    ], text=True)
    for line in plan.splitlines():
        match = re.fullmatch(r"'([^']+)' (\S+) (\d+) SHA256:([0-9a-f]{64})", line)
        if match:
            url, filename, size, digest = match.groups()
            packages[filename.split("_", 1)[0]] = {
                "url": url.replace("http://", "https://", 1), "file": filename, "size": int(size), "sha256": digest
            }
    manifest = list(packages.values())
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        paths = list(pool.map(download, manifest))
    for path in paths:
        subprocess.run(["dpkg-deb", "--extract", str(path), str(prefix)], check=True)
    temporary_manifest = manifest_path.with_suffix(".tmp")
    temporary_manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    temporary_manifest.replace(manifest_path)

    # Rebase only the extracted development files. System pkg-config files retain
    # their existing prefixes and continue to describe installed dependencies.
    for pc in prefix.rglob("*.pc"):
        if pc.is_symlink():
            continue
        text = pc.read_text()
        text = re.sub(r"^prefix=/usr$", "prefix=" + str(prefix / "usr"), text, flags=re.MULTILINE)
        pc.write_text(text)

    architecture = subprocess.check_output(["dpkg-architecture", "-qDEB_HOST_MULTIARCH"], text=True).strip()
    library_dir = prefix / "usr/lib" / architecture
    library_dir.mkdir(parents=True, exist_ok=True)
    for base in (Path("/usr/lib") / architecture, Path("/lib") / architecture):
        for runtime in base.glob("*.so*"):
            local = library_dir / runtime.name
            if not os.path.lexists(local):
                local.symlink_to(runtime)
    print(f"Extracted {len(paths)} packages into {prefix}")


if __name__ == "__main__":
    main()
