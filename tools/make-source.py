#!/usr/bin/env python3
"""Create complete, deterministic corresponding source without local secrets/Git."""
import argparse
import io
import json
import os
from pathlib import Path
import sys
import tarfile
sys.path.insert(0, str(Path(__file__).resolve().parent / 'lib'))
from provenance import ROOT, verify

FILES = ['CMakeLists.txt', 'README.md', 'SECURITY.md', 'AGENTS.md', 'LICENSE.GPL-2', 'LICENSE.GPL-3', '.gitignore']
DIRECTORIES = ['src', 'prototype', 'tests', 'cmake', 'po', 'patches', 'packaging', 'tools', 'docs']

def create(destination, version):
    pin = verify(ROOT / 'upstream/keepassxc')
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', pin['source_date_epoch']))
    prefix = f'keepassxc-gnome-{version}'
    entries = {}
    for name in FILES + DIRECTORIES:
        path = ROOT / name
        for file in sorted(path.rglob('*')) if path.is_dir() else [path]:
            if '__pycache__' in file.parts or file.suffix == '.pyc':
                continue
            if file.is_symlink():
                raise RuntimeError(f'Unexpected source symlink: {file}')
            if file.is_file(): entries[file.relative_to(ROOT).as_posix()] = file
    upstream = ROOT / 'upstream/keepassxc'
    for file in sorted(upstream.rglob('*')):
        relative = file.relative_to(upstream)
        if relative.parts[0] != '.git' and file.is_file():
            entries['upstream/keepassxc/' + relative.as_posix()] = file
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(destination, 'w:xz', format=tarfile.PAX_FORMAT) as archive:
        def add(name, content, mode=0o644):
            info = tarfile.TarInfo(prefix + '/' + name); info.size = len(content)
            info.mtime = epoch; info.mode = mode; info.uid = info.gid = 0; info.uname = info.gname = ''
            archive.addfile(info, io.BytesIO(content))
        for name, file in sorted(entries.items()):
            if name in ['upstream/keepassxc/.source-commit', 'upstream/keepassxc/.gitrev', 'upstream/keepassxc/.version']:
                continue
            add(name, file.read_bytes(), 0o755 if file.stat().st_mode & 0o111 else 0o644)
        for name, value in {'.source-commit': pin['commit'], '.gitrev': pin['commit'][:7], '.version': pin['version']}.items():
            add('upstream/keepassxc/' + name, (value + ('' if name == '.gitrev' else '\n')).encode())
    return destination

if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--version', default='0.1.0~dev1')
    parser.add_argument('--output', type=Path, required=True); args = parser.parse_args()
    if not __import__('re').fullmatch(r'[0-9][0-9A-Za-z.+~:-]*', args.version): parser.error('Invalid version')
    print(create(args.output, args.version))
