#!/usr/bin/env python3
"""Read public upstream metadata; never fetch/checkout/patch/publish code."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import urllib.request

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(); parser.add_argument('--output', type=Path); args = parser.parse_args()
pin = json.loads((root / 'packaging/upstream.json').read_text())
result = {'checked_at': datetime.now(timezone.utc).isoformat(), 'baseline': pin,
          'security_scope': 'Public GitHub metadata only; absence is not a vulnerability assessment.'}
failed = False
for name, suffix in [('latest_release', '/releases/latest'), ('public_advisories', '/security-advisories?per_page=100')]:
    url = 'https://api.github.com/repos/keepassxreboot/keepassxc' + suffix
    request = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'keepassxc-gnome-upstream-check'})
    try:
        with urllib.request.urlopen(request, timeout=20) as response: value = json.load(response)
        if name == 'latest_release':
            result[name] = {key: value.get(key) for key in ('tag_name', 'html_url', 'published_at', 'target_commitish')}
            result['release_differs_from_baseline'] = value.get('tag_name') != pin['tag']
        else:
            result[name] = [{key: item.get(key) for key in ('ghsa_id', 'cve_id', 'severity', 'summary', 'published_at', 'updated_at', 'html_url', 'vulnerabilities')} for item in value]
            result['advisories_may_be_paginated'] = len(value) == 100
    except Exception as error:
        failed = True; result[name] = {'status': 'unknown', 'error': str(error), 'url': url}
text = json.dumps(result, indent=2, ensure_ascii=False) + '\n'
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True); args.output.write_text(text)
print(text, end='')
raise SystemExit(1 if failed else 0)
