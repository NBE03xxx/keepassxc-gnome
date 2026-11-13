#!/usr/bin/env bash
# Optional driver-unload contrast, with no production dlopen change/suppression.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
source tools/dev-env.sh
root="$PWD/build/vulkan-lifetime"
mkdir -p "$root"
library=$(realpath .cache/development/sysroot/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so)
python3 - "$root" "$library" <<'PY'
import hashlib,json,sys
from pathlib import Path
root,library=Path(sys.argv[1]),Path(sys.argv[2])
pin=json.loads(Path('tests/font-offset-audit.json').read_text())
assert hashlib.sha256(library.read_bytes()).hexdigest()==pin['mesa_sha256'],'Mesa changed: re-review expected driver finding'
base=json.loads(Path('/usr/share/vulkan/icd.d/radeon_icd.json').read_text())
base['ICD']['library_path']=str(library)
(root/'icd.json').write_text(json.dumps(base))
PY
g++ -g -fsanitize=address -fno-omit-frame-pointer tests/vulkan_memory_control.cpp -o "$root/control" $(pkg-config --cflags --libs vulkan) -ldl
for scenario in unload-1 unload-10 hold-10; do
    mkdir -p "$root/$scenario"
    repeats=${scenario##*-}
    set +e
    if [[ $scenario == hold-* ]]; then
        KPXC_VULKAN_HOLD_DRIVER="$library" KPXC_VULKAN_REPEATS="$repeats" KPXC_VULKAN_TRACE_ROOT="$root/$scenario" \
            VK_DRIVER_FILES="$root/icd.json" ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 "$root/control" --device >"$root/$scenario/app.log" 2>&1
    else
        env -u KPXC_VULKAN_HOLD_DRIVER KPXC_VULKAN_REPEATS="$repeats" KPXC_VULKAN_TRACE_ROOT="$root/$scenario" \
            VK_DRIVER_FILES="$root/icd.json" ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 "$root/control" --device >"$root/$scenario/app.log" 2>&1
    fi
    status=$?
    set -e
    printf '%s\n' "$status" >"$root/$scenario/status"
done
python3 - "$root" <<'PY'
import json,re,sys
from pathlib import Path
root=Path(sys.argv[1]);result={}
for scenario,n,held in [('unload-1',1,False),('unload-10',10,False),('hold-10',10,True)]:
    folder=root/scenario;log=(folder/'app.log').read_text();status=int((folder/'status').read_text())
    assert 'ERROR: AddressSanitizer:' not in log and 'DEADLYSIGNAL' not in log,'unexpected access error'
    assert log.count('PASS standalone Vulkan device:')==n,'incomplete control'
    assert f'PASS Vulkan repetitions={n} held_driver={int(held)}' in log,'wrong contrast'
    summary=re.search(r'SUMMARY: AddressSanitizer: (\d+) byte\(s\) leaked in (\d+) allocation\(s\)',log)
    if held:assert status==0 and summary is None,'loaded-module contrast has findings'
    else:assert status==1 and summary and tuple(map(int,summary.groups()))==(128*n,n),'unload finding changed; investigate'
    mappings=(folder/'maps-loaded.txt').read_text().splitlines()
    frames=[]
    for address in re.findall(r'#\d+ (0x[0-9a-f]+).*<unknown module>',log):
        value=int(address,16)
        for line in mappings:
            parts=line.split();start,end=[int(s,16) for s in parts[0].split('-')]
            if start<=value<end and len(parts)>=6:
                frames.append({'module':parts[-1],'elf_offset':hex(value-start+int(parts[2],16))})
    result[scenario]={'repetitions':n,'held_module':held,'raw_exit_status':status,
                      'bytes':int(summary[1]) if summary else 0,'allocations':int(summary[2]) if summary else 0,
                      'resolved_module_frames':frames}
result['interpretation']='Observed allocation survives driver unload; keeping module loaded preserves its global root. This control does not change product lifetime or suppress findings.'
(root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
PY
