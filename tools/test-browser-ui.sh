#!/usr/bin/env bash
# Run in an isolated Ubuntu VM with a policy-provisioned Web Store extension.
set -euo pipefail

if [[ $# != 3 ]]; then
    echo "Usage: $0 STAGED_APP FIXTURE_WRITER CHROME" >&2
    exit 2
fi
kpxc_browser_app=$(realpath -e -- "$1")
kpxc_browser_fixture=$(realpath -e -- "$2")
kpxc_browser_chrome=$(realpath -e -- "$3")
kpxc_browser_project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kpxc_browser_test=$(mktemp -d "${KPXC_BROWSER_TEST_PARENT:-/tmp}/kpxc-ui.XXXXXX")
export LANGUAGE="${KPXC_BROWSER_TEST_LANGUAGE:-en}"
export LANG=C.UTF-8
if [[ $LANGUAGE == ja ]]; then export LANG=ja_JP.UTF-8; fi
unset LC_ALL LC_MESSAGES AT_SPI_BUS_ADDRESS DISPLAY CHROME_CONFIG_HOME CHROME_USER_DATA_DIR
export XDG_CONFIG_HOME="$kpxc_browser_test/config" XDG_CACHE_HOME="$kpxc_browser_test/cache"
export XDG_STATE_HOME="$kpxc_browser_test/state" XDG_DATA_HOME="$kpxc_browser_test/data"
export XDG_RUNTIME_DIR="$kpxc_browser_test/rt" HOME="$kpxc_browser_test/home"
export GSETTINGS_BACKEND=memory GTK_A11Y=atspi GDK_BACKEND=wayland WAYLAND_DISPLAY=kpxc-browser-test
mkdir -m 700 -- "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"

dbus-run-session -- bash -eu -o pipefail -c '
    export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
    dbus-update-activation-environment HOME XDG_CONFIG_HOME XDG_CACHE_HOME XDG_STATE_HOME XDG_DATA_HOME         XDG_RUNTIME_DIR GSETTINGS_BACKEND GTK_A11Y LANG LANGUAGE GDK_BACKEND WAYLAND_DISPLAY DBUS_SYSTEM_BUS_ADDRESS
    gnome-shell --headless --no-x11 --wayland-display="$WAYLAND_DISPLAY" --virtual-monitor=1280x800 > "$5/shell.log" 2>&1 &
    kpxc_shell_pid=$!
    trap '"'"'kill "$kpxc_shell_pid" 2>/dev/null || true; wait "$kpxc_shell_pid" 2>/dev/null || true'"'"' EXIT
    for attempt in $(seq 1 100); do
        [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]] && break
        kill -0 "$kpxc_shell_pid" || { cat "$5/shell.log" >&2; exit 1; }
        sleep .1
    done
    [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]]
    python3 "$1" "$2" "$3" "$4" "$5"
' bash "$kpxc_browser_project/tests/browser_pairing_ui.py" "$kpxc_browser_app" "$kpxc_browser_fixture" "$kpxc_browser_chrome" "$kpxc_browser_test"
