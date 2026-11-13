"""Monitor the separate GUI probe; it runs the same Window and actual actions.
Native file-picker plumbing is reserved for the final VM/login-session check.
"""
import os
import time
from pathlib import Path
log = Path(os.environ['KPXC_UI_ROOT']) / 'app.log'
end = time.monotonic() + 45
while time.monotonic() < end:
    data = log.read_text(errors='replace') if log.exists() else ''
    if 'FAIL save UI:' in data:
        raise AssertionError(data)
    if 'PASS save GUI:' in data:
        print(data[data.index('PASS save GUI:'):].strip())
        break
    time.sleep(.1)
else:
    raise AssertionError('Save GUI workflow did not complete: ' + data)
