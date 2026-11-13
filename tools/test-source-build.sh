#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
artifact=${1:-build/dist/keepassxc-gnome-development_0.1.0~dev1-1.source.tar.xz}
work=$(mktemp -d "$PWD/build/source-rebuild.XXXXXX")
python3 - "$artifact" "$work" <<'PY'
import sys,tarfile
with tarfile.open(sys.argv[1]) as archive: archive.extractall(sys.argv[2], filter='data')
PY
mapfile -t roots < <(find "$work" -mindepth 1 -maxdepth 1 -type d)
[[ ${#roots[@]} == 1 ]]
cmake -S "${roots[0]}" -B "$work/build" -G Ninja
CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4} cmake --build "$work/build"
ctest --test-dir "$work/build" --output-on-failure
printf 'PASS complete corresponding source rebuilt and tested: %s\n' "$work"
