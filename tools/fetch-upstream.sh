#!/usr/bin/env bash
set -euo pipefail
kpxc_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kpxc_source="$kpxc_root/upstream/keepassxc"
cd -- "$kpxc_root"
kpxc_tag=$(python3 -c 'import json; print(json.load(open("packaging/upstream.json"))["tag"])')
if [[ ! -e "$kpxc_source" ]]; then
    git clone --depth 1 --branch "$kpxc_tag" https://github.com/keepassxreboot/keepassxc.git "$kpxc_source"
fi
python3 "$kpxc_root/tools/verify-upstream.py" "$kpxc_source"
