#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
if [[ ! -x build/reference/gnome_reference_fixture ]]; then
    echo 'Build the normal project first to prepare the pristine reference fixture tool.' >&2
    exit 1
fi
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
if [[ ${1:-} == --keyfile-ui ]]; then
    cmake --build build/reader-asan --target gnome_keyfile_ui_probe --parallel 4
    bash tools/test-ui.sh --keyfile-asan
    exit
fi
if [[ ${1:-} == --stream-only ]]; then
    cmake --build build/reader-asan --target gnome_stream_finalization_tests --parallel 4
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
        build/reader-asan-output/bin/gnome_stream_finalization_tests
    exit
fi
if [[ ${1:-} == --gtk-control ]]; then
    cmake --build build/reader-asan --target gnome_gtk_memory_control --parallel 4
    bash tools/test-ui.sh --gtk-memory-control
    exit
fi
if [[ ${1:-} == --lifetime-ui ]]; then
    cmake --build build/reader-asan --target gnome_lifetime_ui_probe --parallel 4
    bash tools/test-ui.sh --lifetime-asan
    exit
fi
if [[ ${1:-} == --default-keys-ui ]]; then
    cmake --build build/reader-asan --target gnome_default_keys_ui_probe --parallel 4
    bash tools/test-ui.sh --default-keys-asan
    exit
fi
if [[ ${1:-} == --close-ui ]]; then
    cmake --build build/reader-asan --target gnome_close_ui_probe --parallel 4
    bash tools/test-ui.sh --close-asan
    exit
fi
if [[ ${1:-} == --welcome-ui ]]; then
    cmake --build build/reader-asan --target gnome_welcome_ui_probe gnome_recent_tests --parallel 4
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 build/reader-asan-output/bin/gnome_recent_tests
    bash tools/test-ui.sh --welcome-asan
    exit
fi
if [[ ${1:-} == --polish-ui ]]; then
    cmake --build build/reader-asan --target gnome_polish_ui_probe --parallel 4
    bash tools/test-ui.sh --polish-asan
    exit
fi
if [[ ${1:-} == --picker-only ]]; then
    cmake --build build/reader-asan --target gnome_picker_tests --parallel 4
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
        build/reader-asan-output/bin/gnome_picker_tests "$PWD/build/bin/keepassxc-gnome-picker"
    exit
fi
if [[ ${1:-} == --lifetime-only ]]; then
    cmake --build build/reader-asan --target gnome_lifetime_tests --parallel 4
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
        XDG_CONFIG_HOME="$PWD/build/reader-asan-profile/config" \
        XDG_CACHE_HOME="$PWD/build/reader-asan-profile/cache" \
        XDG_STATE_HOME="$PWD/build/reader-asan-profile/state" \
        build/reader-asan-output/bin/gnome_lifetime_tests "$PWD/build/bin/keepassxc-gnome-worker" "$PWD/build/reference/src/cli/keepassxc-cli"
    exit
fi
cmake --build build/reader-asan --target gnome_stream_finalization_tests gnome_xml_model_tests gnome_known_data_tests gnome_process_known_data_tests gnome_process_tests gnome_save_tests gnome_lifetime_tests --parallel 4
export XDG_CONFIG_HOME="$PWD/build/reader-asan-profile/config"
export XDG_CACHE_HOME="$PWD/build/reader-asan-profile/cache"
export XDG_STATE_HOME="$PWD/build/reader-asan-profile/state"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_stream_finalization_tests
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_xml_model_tests
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_known_data_tests "$PWD/build/reference/gnome_reference_fixture"
# RLIMIT_AS cannot be applied to ASan's enormous virtual shadow reservation.
# Check the instrumented controller against the normal resource-limited worker;
# the direct tests above instrument the actual core reader/model separately.
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_process_known_data_tests "$PWD/build/reference/gnome_reference_fixture" "$PWD/build/bin/keepassxc-gnome-worker"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_process_tests "$PWD/build/bin/keepassxc-gnome-worker" "$PWD/build/bin/gnome_process_fault_helper"

# Instrument transaction I/O and both lifecycle paths; process-death cases run
# in the normal suite because ASan workers cannot obey the 1 GiB AS limit.
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_save_tests "$PWD/build/bin/keepassxc-gnome-worker" --memory

# Private notification bus and clipboard provider, instrumented parent with the
# normal bounded DB worker. No host lock/sleep or clipboard service is used.
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
    build/reader-asan-output/bin/gnome_lifetime_tests "$PWD/build/bin/keepassxc-gnome-worker" "$PWD/build/reference/src/cli/keepassxc-cli"
