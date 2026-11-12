#!/usr/bin/env bash
set -euo pipefail
kpxc_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kpxc_source="$kpxc_root/upstream/keepassxc"
if [[ ! -e "$kpxc_source" ]]; then
    git clone --depth 1 --branch 2.8.0 https://github.com/keepassxreboot/keepassxc.git "$kpxc_source"
fi
[[ $(git -C "$kpxc_source" rev-parse HEAD) == 1c2f23ef5914c0f442a00533def1689857afb5a7 ]] || {
    echo 'The upstream checkout does not match the supported commit.' >&2
    exit 1
}
