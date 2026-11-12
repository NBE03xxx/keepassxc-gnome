#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
kpxc_ui="$PWD/build/ui-test"
mkdir -p "$kpxc_ui"
export KPXC_UI_ROOT="$kpxc_ui"
dbus-run-session -- bash -eu -o pipefail <<'SESSION'
export XDG_CONFIG_HOME="$KPXC_UI_ROOT/config" XDG_CACHE_HOME="$KPXC_UI_ROOT/cache"
export XDG_STATE_HOME="$KPXC_UI_ROOT/state" XDG_DATA_HOME="$KPXC_UI_ROOT/data"
export XDG_RUNTIME_DIR="$KPXC_UI_ROOT/runtime"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export GSETTINGS_BACKEND=memory GTK_A11Y=atspi LANG=ja_JP.UTF-8 LANGUAGE=ja
unset LC_ALL LC_MESSAGES
gnome-shell --headless --no-x11 --wayland-display=kpxc-gnome-test --virtual-monitor=1280x800 > "$KPXC_UI_ROOT/shell.log" 2>&1 &
shell_pid=$!
app_pid=
trap 'if [[ -n "$app_pid" ]]; then kill "$app_pid" 2>/dev/null || true; fi; kill "$shell_pid" 2>/dev/null || true' EXIT
for attempt in $(seq 1 100); do
    [[ -S "$XDG_RUNTIME_DIR/kpxc-gnome-test" ]] && break
    sleep .1
done
export WAYLAND_DISPLAY=kpxc-gnome-test GDK_BACKEND=wayland
build/bin/keepassxc-gnome > "$KPXC_UI_ROOT/app.log" 2>&1 &
app_pid=$!
python3 tests/ui_smoke.py > "$KPXC_UI_ROOT/result.log" 2>&1
cat "$KPXC_UI_ROOT/result.log"
SESSION
