#!/usr/bin/env bash
# Local/CI entry point. Use an isolated Ubuntu 26.04 worker; no installation,
# publication, keys, credentials or upstream changes are performed here.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
release_version=${KPXC_RELEASE_VERSION:-0.1.0~dev1-1}
release_args=()
release_package=keepassxc-gnome-development
if [[ ${KPXC_RELEASE_PUBLIC:-0} == 1 ]]; then
    : "${KPXC_RELEASE_MAINTAINER:?Set the public Debian Maintainer name and email}"
    release_version=${KPXC_RELEASE_VERSION:-0.3.0-1}
    release_package=keepassxc-gnome
    release_args=(--release --maintainer "$KPXC_RELEASE_MAINTAINER")
fi
release_output=${KPXC_RELEASE_OUTPUT:-$(mktemp -d "$PWD/build/release-check.XXXXXX")}
python3 - "$release_output" "$release_version" <<'PY'
from pathlib import Path
import re,sys
p=Path(sys.argv[1]);p.mkdir(parents=True,exist_ok=True)
if any(p.iterdir()):raise SystemExit('Refusing to overwrite existing release artifacts')
if not re.fullmatch(r'[0-9][0-9A-Za-z.+~:-]*',sys.argv[2]):raise SystemExit('Invalid release version')
PY
release_output=$(realpath -- "$release_output")
release_base="${release_package}_${release_version}"
release_arch=$(dpkg-architecture -qDEB_HOST_ARCH)
python3 tools/verify-upstream.py
cmake -S . -B build -G Ninja -DKPXC_GNOME_DEVELOPMENT_UI=OFF
CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4} cmake --build build
bash tools/test-lifetime.sh --all
for mode in '' --cancel --save --lifetime --unlock-keys --polish --guard --welcome --public-welcome --file-open --close --default-keys --keyfile; do
    if [[ -z $mode ]]; then bash tools/test-ui.sh; else bash tools/test-ui.sh "$mode"; fi
done
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --default-keys
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --keyfile
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --close
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --polish
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --file-open
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --welcome
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --public-welcome
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --unlock-keys
KPXC_UI_ORCA_ROOT="$release_output/orca-ja" bash tools/test-ui.sh --orca
KPXC_UI_LANGUAGE=en KPXC_UI_ORCA_ROOT="$release_output/orca-en" bash tools/test-ui.sh --orca
bash tools/test-reader-memory.sh --picker-only
bash tools/test-reader-memory.sh --lifetime-ui
bash tools/test-reader-memory.sh --polish-ui
bash tools/test-reader-memory.sh --welcome-ui
bash tools/test-reader-memory.sh --close-ui
bash tools/test-reader-memory.sh --default-keys-ui
bash tools/test-reader-memory.sh --keyfile-ui
python3 tools/build-deb.py --version "$release_version" --output "$release_output" "${release_args[@]}"
bash tools/test-package.sh "$release_output/${release_base}_${release_arch}.deb"
bash tools/test-source-build.sh "$release_output/${release_base}.source.tar.xz"
bash tools/test-reproducibility.sh "$release_version" "${release_args[@]}"
printf 'Automated checks complete; VM install and physical-session qualification are separate.\n'
