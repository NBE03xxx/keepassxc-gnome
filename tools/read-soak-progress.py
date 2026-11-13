#!/usr/bin/env python3
"""Read only the synthetic soak's numeric metadata, including an in-progress append."""
import json
import argparse
import statistics as st
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('root',type=Path,nargs='?',default=Path(__file__).resolve().parents[1]/'build/gma/soak-3600-vulkan')
root=parser.parse_args().root
rows=[]
for line in (root/'memory.jsonl').read_text().splitlines():
    try:rows.append(json.loads(line))
    except json.JSONDecodeError:pass
assert rows,'no measurements yet'
last=rows[-1];recent=[r for r in rows if r['phase']==last['phase'] and r['elapsed_s']>=last['elapsed_s']-600]
result={'elapsed_min':round(last['elapsed_s']/60,2),'phase':last['phase'],'cycles':last['cycle'],
        'live_mib':round(last['live_bytes']/1048576,3),'rss_mib':round(last['rss_kib']/1024,2),
        'fds':last['fds'],'threads':last['threads'],'mesa_mapped':last['mesa_mapped'],
        'recent_median_live_mib':round(st.median(r['live_bytes'] for r in recent)/1048576,3),
        'recent_samples':len(recent)}
if 'font_cache_clears' in last:result.update(font_cache_clears=last['font_cache_clears'],longest_font_clear_ms=last['longest_font_clear_us']/1000)
print(json.dumps(result))
