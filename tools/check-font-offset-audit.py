#!/usr/bin/env python3
"""Validate the independent offset audit while preserving the failing raw LSan status."""
import argparse
import json
from pathlib import Path
import re
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('root',type=Path)
parser.add_argument('--negative',action='store_true')
args=parser.parse_args()
root=args.root
log=(root/'app.log').read_text(errors='replace')
snapshot=(root/'app.log.snapshot.log').read_text(errors='replace')
audit=(root/'app.log.offset-audit.txt').read_text()
raw_status=int((root/'raw-exit-status').read_text())
assert raw_status==134, f'unexpected raw exit status {raw_status}'
assert 'OFFSET AUDIT:' in log,'audit incomplete'
assert 'ERROR: AddressSanitizer:' not in log,'memory access error'
cycles=re.findall(r'PASS lifetime GUI cycle (\d+)/(\d+):',snapshot)
soak=re.search(r'PASS GUI SOAK: active_seconds=(\d+) idle_seconds=(\d+) cycles=(\d+) requested_active=(\d+) requested_idle=(\d+)',snapshot)
if soak:
    active,idle,completed,requested_active,requested_idle=map(int,soak.groups())
    assert active>=requested_active and idle>=requested_idle and completed>0,'soak duration incomplete'
    cycles=[(str(completed),str(completed))]
assert cycles and int(cycles[-1][0])==int(cycles[-1][1]),'GUI workflow incomplete'
objects=lambda s:{int(a,16):int(n) for a,n in re.findall(r'^(0x[0-9a-f]+) \((\d+) bytes\)$',s,re.M)}
initial=objects(snapshot)
final=objects(log.split('OFFSET AUDIT:',1)[1])
assert initial and final,'raw object report missing'
assert all(initial.get(a)==n for a,n in final.items()),'exit report adds unaudited objects'
summaries=re.findall(r'SUMMARY: AddressSanitizer: (\d+) byte\(s\) leaked in (\d+) allocation\(s\)',snapshot)
assert len(summaries)==1,'snapshot must contain exactly one raw report'
raw_bytes,raw_count=map(int,summaries[0])
assert sum(initial.values())==raw_bytes and len(initial)==raw_count,'raw snapshot accounting mismatch'
values={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',audit.splitlines()[1])}
assert values['objects']==raw_count and values['explained_bytes']+values['unexplained_bytes']==raw_bytes,'audit accounting mismatch'
assert values['raw_lsan']==1,'unsuppressed findings expected in this pinned environment'
if args.negative:
    unknown=[line for line in audit.splitlines() if line.startswith('unexplained ')]
    assert values['unexplained_bytes']==380,'must detect the complete 37-byte control and 343-byte lost Fontconfig object'
    assert any('size=37 direct=1' in line and 'FontOffsetAudit' in line for line in unknown),'application control not detected'
    assert any('size=24 direct=1' in line and 'FcPatternCreate' in line for line in unknown),'actual Fontconfig owner leak not detected'
    outcome='intentional application and Fontconfig leaks detected; not accepted as offset ownership'
else:
    assert values['unexplained_bytes']==0,'unexplained allocations remain'
    outcome='all reported allocations explained by validated live ownership edges'
result={'outcome':outcome,'cycles':int(cycles[-1][1]),'raw_lsan_exit_status':raw_status,'raw_lsan_zero':False,'raw_snapshot_bytes':raw_bytes,'raw_snapshot_allocations':raw_count,'audited_exit_allocations':len(final),'live_offset_owners':values['live_offset_owners'],'live_mesa_owners':values['live_mesa_owners'],'explained_bytes':values['explained_bytes'],'unexplained_bytes':values['unexplained_bytes'],'negative_control':args.negative}
result.update(json.loads((root/'run-config.json').read_text()))
if soak:result.update(measured_active_s=active,measured_idle_s=idle)
(root/'offset-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
