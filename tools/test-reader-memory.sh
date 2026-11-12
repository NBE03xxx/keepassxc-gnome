#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
# Reuse the existing GCC/ASan runtime and project-local development prefix.
cmake -S upstream/keepassxc -B build/reader-asan -G Ninja \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$PWD/cmake/AttachFrontend.cmake" \
    "-DKPXC_GNOME_SOURCE=$PWD" "-DKPXC_GNOME_OUTPUT=$PWD/build/reader-asan-output" \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer' \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address -DCMAKE_SKIP_RPATH=ON \
    -DKPXC_MINIMAL=ON -DKPXC_FEATURE_NETWORK=OFF -DKPXC_FEATURE_DOCS=OFF \
    -DWITH_X11=OFF -DWITH_TESTS=ON -DWITH_GUI_TESTS=OFF
cmake --build build/reader-asan --target gnome_xml_model_tests --parallel 4
export XDG_CONFIG_HOME="$PWD/build/reader-asan-profile/config"
export XDG_CACHE_HOME="$PWD/build/reader-asan-profile/cache"
export XDG_STATE_HOME="$PWD/build/reader-asan-profile/state"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_xml_model_tests
