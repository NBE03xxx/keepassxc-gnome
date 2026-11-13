"""Pinned source verification shared by configuration and artifact creation."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def lock():
    return json.loads((ROOT / 'packaging/upstream.json').read_text())

def tree_digest(folder):
    digest = hashlib.sha256()
    for path in sorted(folder.rglob('*')):
        relative = path.relative_to(folder)
        if relative.parts[0] == '.git' or str(relative) in ('.source-commit', '.gitrev', '.version'):
            continue
        if path.is_symlink():
            raise RuntimeError(f'Unexpected upstream symlink: {relative}')
        if not path.is_file():
            continue
        mode = '755' if path.stat().st_mode & 0o111 else '644'
        line = f'{relative.as_posix()}\0{mode}\0{hashlib.sha256(path.read_bytes()).hexdigest()}\n'
        digest.update(line.encode())
    return digest.hexdigest()

def verify(folder):
    folder = Path(folder).resolve(); pin = lock()
    if (folder / '.git').exists():
        commit = subprocess.check_output(['git', '-C', str(folder), 'rev-parse', 'HEAD'], text=True).strip()
        dirty = subprocess.check_output(['git', '-C', str(folder), 'status', '--porcelain', '--untracked-files=all'], text=True)
        if dirty:
            raise RuntimeError('Upstream checkout has changes; keep it read-only')
    else:
        commit = (folder / '.source-commit').read_text().strip()
        if (folder / '.gitrev').read_bytes() != pin['commit'][:7].encode() or (folder / '.version').read_text().strip() != pin['version']:
            raise RuntimeError('Upstream archive version markers differ from baseline')
    if commit != pin['commit'] or tree_digest(folder) != pin['tree_sha256']:
        raise RuntimeError('Upstream commit or complete source tree differs from reviewed baseline')
    return pin
