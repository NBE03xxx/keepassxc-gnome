#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
python3 tests/package_tests.py "${1:-build/dist/keepassxc-gnome-development_0.1.0~dev1-1_amd64.deb}"
