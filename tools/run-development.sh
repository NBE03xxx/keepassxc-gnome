#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
exec build/bin/keepassxc-gnome "$@"
