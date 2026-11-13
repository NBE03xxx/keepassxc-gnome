#!/usr/bin/env python3
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'lib'))
from provenance import verify
pin = verify(sys.argv[1] if len(sys.argv) == 2 else Path(__file__).resolve().parents[1] / 'upstream/keepassxc')
print(pin['commit'])
