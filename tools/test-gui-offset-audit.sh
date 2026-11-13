#!/usr/bin/env bash
# A version-pinned diagnostic; it does not convert raw LSan failures to zero.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
unset LD_PRELOAD
mode=${1:---positive};cycles=${2:-1};renderer=${3:-default}
[[ $renderer == default || $renderer == cairo || $renderer == vulkan ]]
if [[ $renderer == default ]]; then unset GSK_RENDERER; else export GSK_RENDERER=$renderer; fi
[[ $mode == --positive || $mode == --negative || $mode == --soak ]]
if [[ $mode == --soak ]]; then
    [[ $cycles =~ ^[0-9]+$ && $cycles -ge 60 && $cycles -le 7200 ]]
else [[ $cycles =~ ^([1-9]|1[0-9]|20)$ ]]; fi
python3 - <<'PY'
import hashlib,json
from pathlib import Path
pin=json.loads(Path('tests/font-offset-audit.json').read_text())
library=Path('.cache/development/sysroot/usr/lib/x86_64-linux-gnu/libfontconfig.so.1')
assert hashlib.sha256(library.read_bytes()).hexdigest()==pin['fontconfig_sha256'],'Fontconfig changed: re-review private offset layout before auditing'
mesa=Path('.cache/development/sysroot/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so')
assert hashlib.sha256(mesa.read_bytes()).hexdigest()==pin['mesa_sha256'],'Mesa changed: re-review empty context ownership before auditing'
PY
cmake --build build/reader-asan --target gnome_lifetime_ui_probe --parallel 4
# Wayland's Unix socket path (including the runtime suffix) must fit 108 bytes.
tag=${KPXC_GUI_RUN_TAG:-}
[[ -z $tag || $tag =~ ^[a-z][a-z0-9-]{0,7}$ ]]
root="$PWD/build/gma/${tag:+$tag-}${mode#--}-$cycles-$renderer"
mkdir -p "$root"
python3 - "$root" "$renderer" "$mode" "$cycles" "${4:-900}" <<'PY'
import datetime,hashlib,json,os,sys
from pathlib import Path
paths=['src/main.cpp','src/gnome/FontCache.h','tests/lifetime_ui_probe.cpp','tests/SoakMetrics.h','tests/FontOffsetAudit.h','tools/test-gui-offset-audit.sh','tools/test-ui.sh','tools/check-font-offset-audit.py','tools/analyze-gui-soak.py','build/reader-asan-output/bin/gnome_lifetime_ui_probe','build/bin/keepassxc-gnome-worker']
config={'renderer':sys.argv[2], 'mesa_shader_cache_disable':os.environ.get('MESA_SHADER_CACHE_DISABLE','unset'),
        'test_mode':sys.argv[3], 'run_tag':os.environ.get('KPXC_GUI_RUN_TAG',''), 'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'source_binary_sha256':{p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths},
        'dependency_pin':json.loads(Path('tests/font-offset-audit.json').read_text())}
if sys.argv[3]=='--soak':config.update(requested_active_s=int(sys.argv[4]), requested_idle_s=int(sys.argv[5]),pace_ms=int(os.environ.get('KPXC_GUI_PACE_MS','5000')))
Path(sys.argv[1],'run-config.json').write_text(json.dumps(config,indent=2)+'\n')
PY
export KPXC_GUI_AUDIT_DIR="$root" KPXC_GUI_OFFSET_AUDIT=1 KPXC_GUI_DETECT_LEAKS=1
export KPXC_GUI_CYCLES="$cycles" KPXC_UI_PASS='OFFSET AUDIT:' KPXC_UI_EXIT_ATTEMPTS=1000
unset KPXC_GUI_SOAK_SECONDS KPXC_GUI_IDLE_SECONDS KPXC_UI_TIMEOUT
if [[ $mode == --soak ]]; then
    idle=${4:-900}
    [[ $idle =~ ^[0-9]+$ && $idle -ge 15 && $idle -le 1800 ]]
    export KPXC_GUI_CYCLES=1 KPXC_GUI_SOAK_SECONDS="$cycles" KPXC_GUI_IDLE_SECONDS="$idle"
    export KPXC_UI_TIMEOUT=$((cycles+idle+180))
fi
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1
export LSAN_OPTIONS=report_objects=1
unset KPXC_GUI_OFFSET_NEGATIVE
if [[ $mode == --negative ]]; then export KPXC_GUI_OFFSET_NEGATIVE=1; fi
set +e
bash tools/test-ui.sh --lifetime-asan >"$root/harness.log" 2>&1
status=$?
set -e
printf '%s\n' "$status" >"$root/raw-exit-status"
args=();if [[ $mode == --negative ]]; then args+=(--negative); fi
python3 tools/check-font-offset-audit.py "$root" "${args[@]}"
echo 'Raw LSan remains non-zero; see raw-exit-status and app.log. Above is a separate ownership-audit result.'
