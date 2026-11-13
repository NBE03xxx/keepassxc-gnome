#!/usr/bin/env bash
# Local/CI entry point. Use an isolated Ubuntu 26.04 worker; no installation,
# publication, keys, credentials or upstream changes are performed here.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
python3 tools/verify-upstream.py
cmake -S . -B build -G Ninja
CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4} cmake --build build
bash tools/test-lifetime.sh --all
for mode in '' --cancel --save --lifetime --polish; do
    if [[ -z $mode ]]; then bash tools/test-ui.sh; else bash tools/test-ui.sh "$mode"; fi
done
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --polish
bash tools/test-reader-memory.sh --picker-only
bash tools/test-reader-memory.sh --lifetime-ui
bash tools/test-reader-memory.sh --polish-ui
python3 tools/build-deb.py
bash tools/test-package.sh
bash tools/test-source-build.sh
bash tools/test-reproducibility.sh
printf 'Automated checks complete; VM install and physical-session qualification are separate.\n'
