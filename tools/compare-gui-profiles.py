#!/usr/bin/env python3
import argparse,json,re
from pathlib import Path
parser=argparse.ArgumentParser(description='Compare top allocation contexts; incomplete heap accounting, not an LSan acceptance.')
parser.add_argument('root',type=Path)
parser.add_argument('--from-minute',type=float,default=0)
args=parser.parse_args()
s=(args.root/'app.log').read_text(errors='replace')
profiles=[]
for part in re.split(r'ALLOCATION PROFILE ',s)[1:]:
    header=part.splitlines()[0]
    old=re.search(r'minute=(\d+)',header)
    elapsed=re.search(r'elapsed_s=(\d+)',header)
    if not old and not elapsed:continue
    seconds=int(elapsed[1]) if elapsed else int(old[1])*60
    phase=re.search(r'phase=([a-z-]+)',header)
    phase=phase[1] if phase else 'timer'
    clears=re.search(r'clears=(\d+)',header)
    contexts={}
    for m in re.finditer(r'^(\d+) byte\(s\) \(\d+%\) in (\d+) allocation\(s\)\n(.*?)(?=\n\n|\Z)',part,re.M|re.S):
        stack=re.sub(r'(#\d+) 0x[0-9a-f]+ ',r'\1 ',m[3]);contexts[stack]={'bytes':int(m[1]),'allocations':int(m[2])}
    total=re.search(r'Live Heap Allocations: (\d+) bytes in (\d+) chunks;',part)
    if not total:continue
    profiles.append({'elapsed_s':seconds,'phase':phase,'live_bytes':int(total[1]),
                     'font_cache_clears':int(clears[1]) if clears else None,
                     'reported_context_bytes':sum(v['bytes'] for v in contexts.values()),'contexts':contexts})
summary=[{k:v for k,v in p.items() if k!='contexts'} for p in profiles]
locked=[p for p in profiles if p['phase'] in ('cycle-locked','timer') and p['elapsed_s']>=args.from_minute*60]
if len(locked)>1:
    first,last=locked[0]['contexts'],locked[-1]['contexts']
    rows=[]
    for stack in set(first)|set(last):
        before=first.get(stack,{}).get('bytes',0);after=last.get(stack,{}).get('bytes',0)
        frames=stack.strip().splitlines()
        rows.append({'delta_bytes':after-before if stack in first and stack in last else None,
                     'before':before if stack in first else None,'after':after if stack in last else None,
                     'caller':frames[1].strip() if len(frames)>1 else frames[0], 'stack':stack})
    rows.sort(key=lambda r:r['delta_bytes'] if r['delta_bytes'] is not None else -float('inf'),reverse=True)
    (args.root/'profile-comparison.json').write_text(json.dumps({'first_elapsed_s':locked[0]['elapsed_s'],
        'last_elapsed_s':locked[-1]['elapsed_s'],'snapshots':summary,
        'scope':'reported top contexts, same completed locked phase; absence means outside report, not zero; not complete allocation accounting or LSan acceptance','rows':rows},indent=2)+'\n')
    print(json.dumps(summary))
    for r in rows[:12]:print(r['delta_bytes'],r['before'],r['after'],r['caller'])
else:print('Waiting for second profile')
