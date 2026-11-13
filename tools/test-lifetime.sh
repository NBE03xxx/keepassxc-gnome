#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
# The test owns a private D-Bus daemon; it never uses the host sleep/lock API.
if [[ ${1:-} == --all ]]; then
    ctest --test-dir build --output-on-failure
else
    ctest --test-dir build -R '^gnome_lifetime_tests$' --output-on-failure
fi
