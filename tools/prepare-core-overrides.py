#!/usr/bin/env python3
"""Apply the audited patch to build-only copies, keeping upstream read-only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
from provenance import verify


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--patch-tool", required=True)
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    manifest = json.loads((project / "patches/reader-finalization.json").read_text())
    upstream, output = args.upstream.resolve(), args.output.resolve()
    if upstream == output or upstream in output.parents or output in upstream.parents:
        raise RuntimeError("Overrides must be outside the reference checkout")
    commit = verify(upstream)["commit"]
    if commit != manifest["commit"]:
        raise RuntimeError("Upstream commit differs from the reviewed patch baseline")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="core-overrides-", dir=output.parent) as temporary:
        work = Path(temporary)
        for source, digest in manifest["files"].items():
            original = upstream / source
            if hashlib.sha256(original.read_bytes()).hexdigest() != digest:
                raise RuntimeError(f"Upstream source differs from the reviewed file: {source}")
            target = work / source
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
        for patch in manifest["patches"]:
            subprocess.run([args.patch_tool, "--batch", "--fuzz=0", "-p1", "--directory", str(work),
                            "--input", str(project / "patches" / patch)], check=True)
        for source in manifest["files"]:
            target = output / source
            patched = (work / source).read_bytes()
            if not target.exists() or target.read_bytes() != patched:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(patched)


if __name__ == "__main__":
    main()
