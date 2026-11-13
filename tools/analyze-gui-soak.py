#!/usr/bin/env python3
"""Summarize fixed-phase metadata; thresholds flag investigation, not a proof of all lifetimes."""
import argparse
from decimal import Decimal
import json
import statistics as st
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
p.add_argument('--pilot',action='store_true')
p.add_argument('--require-font-cache',action='store_true',help='also require the production five-minute maintenance to run')
a=p.parse_args()
rows=[json.loads(s) for s in (a.root/'memory.jsonl').read_text().splitlines()]
assert rows and all(r['rss_kib']>0 and r['live_bytes']>0 for r in rows),'missing process/ASan metadata'
audit=json.loads((a.root/'offset-result.json').read_text())
assert audit['unexplained_bytes']==0 and not audit['negative_control'],'unexplained leak report'
active=[r for r in rows if r['phase']=='cycle-locked']
idle=[r for r in rows if r['phase'] in ('idle','idle-end')]
assert active and idle and rows[-1]['phase']=='finalized','incomplete active/idle/finalization evidence'
assert all(r['mesa_mapped']==1 for r in active+idle),'driver disappeared between samples'
assert [r['cycle'] for r in active]==list(range(1,len(active)+1)),'missing cycle samples'
active_s=active[-1]['elapsed_s']-rows[0]['elapsed_s'] if len(active)>1 else 0
# Preserve the recorded decimal timestamps: 4501.48 - 3601.48 is 900,
# not the binary-float approximation 899.9999999999995. No threshold slack.
idle_s=float(Decimal(str(idle[-1]['elapsed_s']))-Decimal(str(idle[0]['elapsed_s'])))
if not a.pilot:
    assert audit['measured_active_s']>=3600 and audit['measured_idle_s']>=900,'incomplete monotonic probe duration'
    assert active[-1]['elapsed_s']>=3600 and idle_s>=900,'less than 60 active + 15 idle minutes'
warmup=600 if not a.pilot else 0
steady=[r for r in active if r['elapsed_s']>=warmup]
assert len(steady)>=2,'insufficient steady-phase samples'
period=600 if not a.pilot else 30
buckets=[]
for window in sorted({int(r['elapsed_s']//period) for r in steady}):
    group=[r for r in steady if int(r['elapsed_s']//period)==window]
    buckets.append({'start_s':window*period,'samples':len(group),**{k:st.median(r[k] for r in group) for k in ('live_bytes','rss_kib','fds','threads')}})
# Partial trailing buckets must not dominate the comparison.
full=[b for b in buckets if b['samples']>=max(2,int(period/5*.8))] if not a.pilot else buckets
assert len(full)>=2,'insufficient complete measurement windows'
x=[r['elapsed_s']/60 for r in steady];y=[r['live_bytes'] for r in steady]
xmean,ymean=st.mean(x),st.mean(y)
slope=sum((u-xmean)*(v-ymean) for u,v in zip(x,y))/sum((u-xmean)**2 for u in x)
growth=full[-1]['live_bytes']-full[0]['live_bytes']
limit=max(1024*1024,full[0]['live_bytes']*.05)
flags=[]
if growth>limit:flags.append('live allocation window medians grew beyond 1 MiB / 5% investigation threshold')
if slope>64*1024:flags.append('post-warmup live allocation trend exceeds 64 KiB/min')
if full[-1]['fds']>full[0]['fds']+2:flags.append('descriptor count grew by more than 2')
if full[-1]['threads']>full[0]['threads']+2:flags.append('thread count grew by more than 2')
result={'scope':'pilot only' if a.pilot else '60-minute synthetic GUI workload + 15-minute locked idle',
        'cycles':len(active),'last_active_sample_s':active[-1]['elapsed_s'],'locked_idle_s':idle_s,
        'warmup_excluded_s':warmup,'windows':buckets,'comparison_windows':full,
        'post_warmup_live_slope_bytes_per_minute':slope,'window_median_live_growth_bytes':growth,
        'investigation_growth_threshold_bytes':limit,'investigation_flags':flags,
        'rss_is_not_live_allocation_size':True,'driver_mapped_at_all_samples':True,
        'ending_live_bytes':rows[-1]['live_bytes'],'raw_lsan_zero':False,
        'unexplained_lsan_bytes':audit['unexplained_bytes'],
        'scope_limit':'sampled finite workload; does not prove all operation paths or unbounded future time'}
if a.require_font_cache:
    counts=[r['font_cache_clears'] for r in rows]
    assert counts==sorted(counts),'maintenance counter moved backwards'
    total=rows[-1]['elapsed_s']
    if total>=300:
        assert counts[-1]>=max(1,int(total//300)-1),'scheduled maintenance did not run'
        first=next(r for r in rows if r['font_cache_clears'])
        assert first['elapsed_s']>=299,'maintenance ran early or was forced by a test'
    assert counts[-1]<=int(total//300)+1,'maintenance ran too frequently'
    result.update(font_cache_clears=counts[-1],longest_font_clear_ms=rows[-1]['longest_font_clear_us']/1000,
                  maintenance_is_observed_production_timer=True)
(a.root/'soak-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
raise SystemExit(bool(flags))
