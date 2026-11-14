#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
kpxc_ui="$PWD/build/ui-test"
export KPXC_UI_APP="$PWD/build/bin/keepassxc-gnome"
export KPXC_UI_SCRIPT="$PWD/tests/ui_smoke.py"
if [[ ${1:-} == --orca ]]; then
    command -v orca >/dev/null || { echo 'Orca is required; consult before installing it.' >&2; exit 1; }
    kpxc_ui="$PWD/build/ui-orca-${KPXC_UI_LANGUAGE:-ja}"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_orca.py" KPXC_UI_EXPECT_EXIT=1 KPXC_UI_ORCA=1
    if [[ -n ${2:-} ]]; then
        export KPXC_UI_APP="$(realpath -- "$2")"
        [[ -x $KPXC_UI_APP ]]
        kpxc_ui+="-package"
    fi
elif [[ ${1:-} == --package ]]; then
    kpxc_ui="$PWD/build/ui-package-test"
    export KPXC_UI_APP="$(realpath -- "${2:?staged package launcher required}")"
    [[ -x $KPXC_UI_APP ]]
elif [[ ${1:-} == --guard ]]; then
    kpxc_ui="$PWD/build/ui-guard-${KPXC_UI_LANGUAGE:-ja}"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_guard.py" KPXC_UI_GUARD_FAILURE=1
    if [[ -n ${2:-} ]]; then
        export KPXC_UI_APP="$(realpath -- "$2")"
        [[ -x $KPXC_UI_APP ]]
        kpxc_ui+="-package"
    fi
elif [[ ${1:-} == --cancel ]]; then
    # A copy of the real UI plus a fault helper; never install or replace the
    # application's real worker. This has no production environment hook.
    kpxc_ui="$PWD/build/ui-cancel-test"
    mkdir -p "$kpxc_ui/bin" "$kpxc_ui/share"
    cp build/bin/keepassxc-gnome "$kpxc_ui/bin/keepassxc-gnome"
    cp build/bin/keepassxc-gnome-picker "$kpxc_ui/bin/keepassxc-gnome-picker"
    cp build/bin/gnome_process_fault_helper "$kpxc_ui/bin/keepassxc-gnome-worker"
    cp -a build/share/locale "$kpxc_ui/share/"
    export KPXC_UI_APP="$kpxc_ui/bin/keepassxc-gnome"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_cancel.py"
    export KPXC_UI_EXPECT_EXIT=1
elif [[ ${1:-} == --gtk-font-shutdown ]]; then
    kpxc_ui="$PWD/build/ui-gtk-font-shutdown"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_lifetime.py"
    export KPXC_UI_APP="$PWD/build/gui-memory-audit/gtk-font-shutdown-control"
    export KPXC_UI_EXPECT_EXIT=1 KPXC_UI_PASS="PASS GTK control:"
    export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1
elif [[ ${1:-} == --gtk-memory-control ]]; then
    kpxc_ui="$PWD/build/ui-gtk-memory-control"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_lifetime.py"
    export KPXC_UI_APP="$PWD/build/reader-asan-output/bin/gnome_gtk_memory_control"
    export KPXC_UI_EXPECT_EXIT=1 KPXC_UI_PASS="PASS GTK control:"
    export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1
elif [[ ${1:-} == --lifetime-asan ]]; then
    kpxc_ui="${KPXC_GUI_AUDIT_DIR:-$PWD/build/ui-lifetime-asan}"
    mkdir -p "$kpxc_ui/bin" "$kpxc_ui/share"
    cp build/reader-asan-output/bin/gnome_lifetime_ui_probe "$kpxc_ui/bin/gnome_lifetime_ui_probe"
    cp build/bin/keepassxc-gnome-worker "$kpxc_ui/bin/keepassxc-gnome-worker"
    cp -a build/share/locale "$kpxc_ui/share/"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_lifetime.py"
    export KPXC_UI_APP="$kpxc_ui/bin/gnome_lifetime_ui_probe"
    export KPXC_UI_EXPECT_EXIT=1
    # The toolkit-only control also reports font shutdown allocations. Check
    # GUI accesses here; LSan remains enabled in non-rendering lifetime tests.
    export ASAN_OPTIONS=detect_leaks=${KPXC_GUI_DETECT_LEAKS:-0}:abort_on_error=1:strict_string_checks=1
elif [[ ${1:-} == --lifetime ]]; then
    kpxc_ui="$PWD/build/ui-lifetime-test"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_lifetime.py"
    export KPXC_UI_APP="$PWD/build/bin/gnome_lifetime_ui_probe"
    export KPXC_UI_EXPECT_EXIT=1
elif [[ ${1:-} == --polish-asan ]]; then
    kpxc_ui="$PWD/build/ui-polish-asan"
    mkdir -p "$kpxc_ui/bin" "$kpxc_ui/share"
    cp build/reader-asan-output/bin/gnome_polish_ui_probe "$kpxc_ui/bin/gnome_polish_ui_probe"
    cp build/bin/keepassxc-gnome-worker build/bin/keepassxc-gnome-picker "$kpxc_ui/bin/"
    cp -a build/share/locale "$kpxc_ui/share/"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_lifetime.py"
    export KPXC_UI_APP="$kpxc_ui/bin/gnome_polish_ui_probe"
    export KPXC_UI_EXPECT_EXIT=1 KPXC_UI_PASS="PASS polish GUI:"
    export ASAN_OPTIONS=detect_leaks=${KPXC_GUI_DETECT_LEAKS:-0}:abort_on_error=1:strict_string_checks=1
elif [[ ${1:-} == --polish ]]; then
    kpxc_ui="$PWD/build/ui-polish-${KPXC_UI_LANGUAGE:-ja}"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_lifetime.py"
    export KPXC_UI_APP="$PWD/build/bin/gnome_polish_ui_probe"
    export KPXC_UI_EXPECT_EXIT=1 KPXC_UI_PASS="PASS polish GUI:"
elif [[ ${1:-} == --save ]]; then
    kpxc_ui="$PWD/build/ui-save-test"
    export KPXC_UI_SCRIPT="$PWD/tests/ui_save.py"
    export KPXC_UI_APP="$PWD/build/bin/gnome_save_ui_probe"
    export KPXC_UI_EXPECT_EXIT=1
fi
mkdir -p "$kpxc_ui"
export KPXC_UI_ROOT="$kpxc_ui"
dbus-run-session -- bash -eu -o pipefail <<'SESSION'
# Keep system-bus calls inside this disposable session as well. Sleep-delay
# inhibitor and notification contracts are tested with a private mock manager.
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
export XDG_CONFIG_HOME="$KPXC_UI_ROOT/config" XDG_CACHE_HOME="$KPXC_UI_ROOT/cache"
export XDG_STATE_HOME="$KPXC_UI_ROOT/state" XDG_DATA_HOME="$KPXC_UI_ROOT/data"
export XDG_RUNTIME_DIR
XDG_RUNTIME_DIR=$(mktemp -d "$KPXC_UI_ROOT/runtime.XXXXXX")
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export GSETTINGS_BACKEND=memory GTK_A11Y=atspi LANG=ja_JP.UTF-8 LANGUAGE="${KPXC_UI_LANGUAGE:-ja}"
unset LC_ALL LC_MESSAGES AT_SPI_BUS_ADDRESS DISPLAY
export WAYLAND_DISPLAY=kpxc-gnome-test GDK_BACKEND=wayland
# Update this disposable bus only. Activated portals and accessibility services
# must inherit the private directories/display instead of the login session.
dbus-update-activation-environment XDG_CONFIG_HOME XDG_CACHE_HOME XDG_STATE_HOME XDG_DATA_HOME XDG_RUNTIME_DIR GSETTINGS_BACKEND GTK_A11Y LANG LANGUAGE WAYLAND_DISPLAY GDK_BACKEND DBUS_SYSTEM_BUS_ADDRESS
kpxc_shell_options=()
# Test-only window activation on this disposable bus; never enable this flag
# on the login session. No application dump protection is changed.
if [[ ${KPXC_UI_ORCA:-0} == 1 ]]; then kpxc_shell_options+=(--unsafe-mode); fi
gnome-shell "${kpxc_shell_options[@]}" --headless --no-x11 --wayland-display=kpxc-gnome-test --virtual-monitor=1280x800 > "$KPXC_UI_ROOT/shell.log" 2>&1 &
shell_pid=$!
app_pid=
orca_pid=
trap 'if [[ -n "$app_pid" ]]; then kill "$app_pid" 2>/dev/null || true; fi; if [[ -n "$orca_pid" ]]; then kill "$orca_pid" 2>/dev/null || true; wait "$orca_pid" 2>/dev/null || true; fi; kill "$shell_pid" 2>/dev/null || true' EXIT
for attempt in $(seq 1 100); do
    [[ -S "$XDG_RUNTIME_DIR/kpxc-gnome-test" ]] && break
    kill -0 "$shell_pid" || { echo "Private GNOME session stopped during startup." >&2; exit 1; }
    sleep .1
done
[[ -S "$XDG_RUNTIME_DIR/kpxc-gnome-test" ]] || { echo "Private Wayland socket did not become ready." >&2; exit 1; }
if [[ ${KPXC_UI_ORCA:-0} == 1 ]]; then
    mkdir -p "$XDG_DATA_HOME/orca"
    cp tests/orca_settings.py "$XDG_DATA_HOME/orca/orca-customizations.py"
    gdbus wait --session --timeout 15 org.gnome.Shell
    orca --debug-file "$KPXC_UI_ROOT/orca-debug.log" >"$KPXC_UI_ROOT/orca.log" 2>&1 &
    orca_pid=$!
fi
if [[ ${KPXC_UI_GUARD_FAILURE:-0} == 1 ]]; then
    # Only this test application's system-bus connection is made unreachable.
    # GNOME, accessibility and portals retain the working disposable bus.
    env DBUS_SYSTEM_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/missing-system-bus" "$KPXC_UI_APP" > "$KPXC_UI_ROOT/app.log" 2>&1 &
else
    "$KPXC_UI_APP" > "$KPXC_UI_ROOT/app.log" 2>&1 &
fi
app_pid=$!
export KPXC_UI_PID="$app_pid"
python3 "$KPXC_UI_SCRIPT" > "$KPXC_UI_ROOT/result.log" 2>&1
cat "$KPXC_UI_ROOT/result.log"
if [[ ${KPXC_UI_EXPECT_EXIT:-0} == 1 ]]; then
    for attempt in $(seq 1 "${KPXC_UI_EXIT_ATTEMPTS:-100}"); do
        kill -0 "$app_pid" 2>/dev/null || break
        sleep .02
    done
    if kill -0 "$app_pid" 2>/dev/null; then
        echo 'Application did not exit after closing during a blocked request.' >&2
        exit 1
    fi
    wait "$app_pid"
    app_pid=
fi
SESSION
