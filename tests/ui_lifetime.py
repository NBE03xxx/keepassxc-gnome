"""Monitor the separate GUI probe; it runs the same Window and actual actions.
Native file-picker plumbing is reserved for the final VM/login-session check.
"""
import os
import time
from pathlib import Path
log = Path(os.environ['KPXC_UI_ROOT']) / 'app.log'
expected = os.environ.get('KPXC_UI_PASS', 'PASS lifetime GUI:')
timeout = int(os.environ.get('KPXC_UI_TIMEOUT', '45'))
assert 45 <= timeout <= 10800, 'bounded private GUI monitor timeout'
end = time.monotonic() + timeout
while time.monotonic() < end:
    data = log.read_text(errors='replace') if log.exists() else ''
    if any(marker in data for marker in ('FAIL lifetime UI:', 'FAIL polish UI:', 'FAIL close UI:', 'FAIL welcome UI:')):
        raise AssertionError(data)
    if expected in data:
        print(data[data.index(expected):].splitlines()[0])
        break
    time.sleep(1 if timeout > 300 else .1)
else:
    raise AssertionError('Lifetime GUI workflow did not complete: ' + data)
