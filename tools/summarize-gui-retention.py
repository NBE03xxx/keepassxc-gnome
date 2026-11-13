#!/usr/bin/env python3
"""Selected allocation-site lower bounds in a fixed-build diagnostic, not heap/LSan completeness."""
import argparse,json,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
a=p.parse_args()
snapshots=[]
for part in (a.root/'app.log').read_text(errors='replace').split('ALLOCATION PROFILE ')[1:]:
    header=part.splitlines()[0]
    total=re.search(r'Live Heap Allocations: (\d+) bytes in (\d+) chunks;',part)
    if not total:continue
    sites={name:{'bytes':0,'allocations':0} for name in ('cairo_glyph_pages','gtk_cached_glyph','fc_pattern_duplicate')}
    reported=0;contexts=0
    for m in re.finditer(r'^(\d+) byte\(s\) \(\d+%\) in (\d+) allocation\(s\)\n(.*?)(?=\n\n|\Z)',part,re.M|re.S):
        size,n,stack=int(m[1]),int(m[2]),m[3];reported+=size;contexts+=1
        selected=[]
        if 'libcairo.so.2+0x75494)' in stack and '05177818680003739ec6cb6b7b3a77249b1aa633' in stack:selected.append('cairo_glyph_pages')
        if 'libgtk-4.so.1+0x63045c)' in stack and '4c74ae5215a17f78b116635961e03231361bf671' in stack:selected.append('gtk_cached_glyph')
        if 'in FcPatternDuplicate ' in stack:selected.append('fc_pattern_duplicate')
        for name in selected:sites[name]['bytes']+=size;sites[name]['allocations']+=n
    assert reported<=int(total[1]),'reported contexts exceed heap total'
    snapshots.append({'sample':header,'live_bytes':int(total[1]),'reported_contexts':contexts,
                      'reported_context_bytes':reported,'reported_fraction':reported/int(total[1]),'sites':sites})
result={'scope':'selected reported allocation contexts only; categories may overlap and absence is not zero; not an LSan acceptance or total-memory upper bound','snapshots':snapshots}
(a.root/'retention-summary.json').write_text(json.dumps(result,indent=2)+'\n')
for row in snapshots:print(row['sample'],row['live_bytes'],row['sites'])
