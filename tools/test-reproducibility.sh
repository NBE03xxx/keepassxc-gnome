#!/usr/bin/env bash
# Byte identity of assembly from identical source/binaries, not independent ELF builds.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
work=$(mktemp -d "$PWD/build/repro-check.XXXXXX")
python3 tools/build-deb.py --output "$work/first"
python3 tools/build-deb.py --output "$work/second"
for item in "$work/first/"*; do
    cmp -- "$item" "$work/second/$(basename -- "$item")"
done
printf 'PASS identical source/deb/manifest/checksums from identical inputs: %s\n' "$work"
