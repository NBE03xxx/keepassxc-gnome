#!/usr/bin/env python3
"""Summarize raw LSan reports without suppressing or accepting library leaks.
Exit 0: no sanitizer report and a completed GUI workflow; 1: findings;
2: incomplete/unrecognized evidence. Origin is attribution, not exoneration.
"""
import argparse
import collections
import json
from pathlib import Path
import re

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('log',type=Path)
parser.add_argument('--output',type=Path)
args=parser.parse_args();data=args.log.read_text(errors='replace')
summary=re.search(r'SUMMARY: AddressSanitizer: (\d+) byte\(s\) leaked in (\d+) allocation\(s\)',data)
groups=[]
for block in re.split(r'\n(?=(?:Direct|Indirect) leak of )',data)[1:]:
    header=re.match(r'(Direct|Indirect) leak of (\d+) byte\(s\) in (\d+) object\(s\)',block)
    if header is None:continue
    frames=re.findall(r'^\s*#\d+ (.*)$',block,re.M)
    origin=frames[1] if len(frames)>1 else '<unresolved>'
    module=re.search(r'\(([^()]+?)(?:\+0x[0-9a-f]+)?\)',origin)
    source=Path(module[1]).name if module else origin
    groups.append({'kind':header[1],'bytes':int(header[2]),'objects':int(header[3]),'origin':source,'first_caller':origin})
by_origin=collections.defaultdict(lambda:{'bytes':0,'objects':0,'groups':0})
for row in groups:
    totals=by_origin[row['origin']]
    for key in ('bytes','objects'):totals[key]+=row[key]
    totals['groups']+=1
cycles=re.findall(r'PASS lifetime GUI cycle (\d+)/(\d+):',data)
complete=bool(cycles and cycles[-1][0]==cycles[-1][1]) or 'PASS GTK control:' in data
invalid=('FAIL lifetime UI:' in data or 'LeakSanitizer has encountered a fatal error' in data)
findings=any(marker in data for marker in ('ERROR: AddressSanitizer:', 'ERROR: LeakSanitizer:', 'AddressSanitizer:DEADLYSIGNAL'))
result={'log':str(args.log.resolve()),'workflow_completed':complete and not invalid,'sanitizer_findings':findings,'origins':dict(by_origin),'attribution_is_not_proof_of_ownership':True}
if summary:
    result['leaked_bytes']=int(summary[1]);result['leaked_allocations']=int(summary[2])
    result['accounting_matches']=sum(r['bytes'] for r in groups)==int(summary[1]) and sum(r['objects'] for r in groups)==int(summary[2])
status=2 if not complete or invalid or (summary and not result['accounting_matches']) else 1 if findings else 0
result['exit_status']=status
text=json.dumps(result,ensure_ascii=False,indent=2)+'\n'
if args.output:args.output.write_text(text)
print(text,end='');raise SystemExit(status)
