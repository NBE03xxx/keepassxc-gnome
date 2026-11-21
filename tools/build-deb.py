#!/usr/bin/env python3
"""Stage a deb and corresponding source; never install or publish anything."""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent / 'lib'))
from provenance import ROOT, verify

NAME = 'keepassxc-gnome-development'
BINARIES = ['keepassxc-gnome', 'keepassxc-gnome-worker', 'keepassxc-gnome-picker', 'keepassxc-gnome-browser-host']

def run(args, **kwargs):
    return subprocess.check_output([str(a) for a in args], text=True, **kwargs).strip()

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--build', type=Path, default=ROOT / 'build')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/dist')
    parser.add_argument('--release', action='store_true', help='Package a public-facing build; does not publish it')
    parser.add_argument('--version')
    parser.add_argument('--maintainer', help='Debian Maintainer name and email; required for --release')
    args = parser.parse_args()
    args.version = args.version or ('0.3.0-1' if args.release else '0.1.0~dev1-1')
    if args.release and not args.maintainer: parser.error('--release requires an explicitly supplied --maintainer')
    args.maintainer = args.maintainer or 'KeePassXC GNOME developers <noreply@localhost>'
    name = 'keepassxc-gnome' if args.release else NAME
    if not re.fullmatch(r'[0-9][0-9A-Za-z.+~:-]*', args.version): parser.error('Invalid Debian version')
    if '\n' in args.maintainer or not re.fullmatch(r'[^<>]+<[^<>\s]+@[^<>\s]+>', args.maintainer): parser.error('Invalid maintainer')
    pin = verify(ROOT / 'upstream/keepassxc'); epoch = int(os.environ.get('SOURCE_DATE_EPOCH', pin['source_date_epoch']))
    os.environ['SOURCE_DATE_EPOCH'] = str(epoch)
    build = args.build.resolve(); output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    cache_values = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        match = re.fullmatch(r'([A-Za-z0-9_]+):[^=]+=(.*)', line)
        if match: cache_values[match[1]] = match[2]
    if (Path(cache_values.get('CMAKE_HOME_DIRECTORY', '/')).resolve() != ROOT
        or Path(cache_values.get('KEEPASSXC_SOURCE_DIR', '/')).resolve() != ROOT / 'upstream/keepassxc'):
        raise RuntimeError('Build belongs to a different source/baseline; reconfigure this project before packaging')
    development_ui = cache_values.get('KPXC_GNOME_DEVELOPMENT_UI', '').upper() in ('ON', 'TRUE', '1')
    if args.release and development_ui:
        raise RuntimeError('Release packaging requires -DKPXC_GNOME_DEVELOPMENT_UI=OFF')
    # A source archive must describe the actual binary, including uncommitted
    # implementation changes. Bring this build up to date before snapshotting.
    subprocess.run(['cmake', '--build', str(build), '--parallel', '4'], check=True)
    architecture = run(['dpkg-architecture', '-qDEB_HOST_ARCH']); multiarch = run(['dpkg-architecture', '-qDEB_HOST_MULTIARCH'])
    spec = importlib.util.spec_from_file_location('source_archive', ROOT / 'tools/make-source.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    source = module.create(output / f'{name}_{args.version}.source.tar.xz', args.version)
    with tempfile.TemporaryDirectory(prefix='deb-', dir=output) as temp:
        work = Path(temp); stage = work / 'debian' / name
        env = dict(os.environ, DESTDIR=str(stage))
        subprocess.run(['cmake', '--install', str(build), '--prefix', '/usr'], env=env, check=True)
        control = stage / 'DEBIAN'; control.mkdir()
        doc = stage / 'usr/share/doc' / name; doc.mkdir(parents=True)
        for license_name in ['LICENSE.GPL-2', 'LICENSE.GPL-3']:
            shutil.copyfile(ROOT / license_name, doc / license_name)
        shutil.copyfile(ROOT / 'packaging/copyright', doc / 'copyright')
        for path in (ROOT / 'upstream/keepassxc').glob('LICENSE.*'):
            shutil.copyfile(path, doc / ('upstream-' + path.name))
        shutil.copyfile(ROOT / 'upstream/keepassxc/COPYING', doc / 'upstream-COPYING')
        shutil.copyfile('/usr/share/common-licenses/Apache-2.0', doc / 'upstream-LICENSE.Apache-2.0')
        docs = ['user-guide.md', 'distribution-maintenance.md'] if args.release else ['phase3-instructions.md', 'distribution-maintenance.md', 'vm-package-test.md']
        for document in docs:
            shutil.copyfile(ROOT / 'docs' / document, doc / document)
        shutil.copyfile(ROOT / 'README.md', doc / 'README.md')
        change = 'Release for Ubuntu 26.04.' if args.release else 'Private development validation build.'
        (doc / 'changelog.gz').write_bytes(gzip.compress((f'{name} ({args.version}) unstable; urgency=medium\n\n  * {change}\n').encode(), mtime=0))
        executables = [stage / 'usr/libexec/keepassxc-gnome' / name for name in BINARIES]
        for path in executables:
            subprocess.run(['strip', '--strip-unneeded', str(path)], check=True)
            dynamic = run(['readelf', '-d', path]); program = run(['readelf', '-lW', path]); header = run(['readelf', '-h', path])
            stack = next((line for line in program.splitlines() if 'GNU_STACK' in line), '')
            if ('RPATH' in dynamic or 'RUNPATH' in dynamic or 'libasan' in dynamic or not stack or 'RWE' in stack
                or 'GNU_RELRO' not in program or 'BIND_NOW' not in dynamic or not re.search(r'Type:\s+DYN', header)):
                raise RuntimeError('Unsafe or instrumented binary in package')
        # Use Ubuntu's metadata for cached libraries that are deliberately not
        # installed on the host. Never ignore missing shlibs information.
        cache = ROOT / '.cache/development'; packages = []
        local_shlibs = []
        if (cache / 'packages.json').exists():
            packages = json.loads((cache / 'packages.json').read_text())
            for item in packages:
                deb = cache / 'packages' / item['file']
                if sha(deb) != item['sha256']: raise RuntimeError('Cached package hash mismatch')
                control_tar = subprocess.check_output(['dpkg-deb', '--ctrl-tarfile', str(deb)])
                with tarfile.open(fileobj=io.BytesIO(control_tar)) as archive:
                    for info in archive.getmembers():
                        if info.name.lstrip('./') == 'shlibs': local_shlibs.append(archive.extractfile(info).read().decode())
        debian = work / 'debian'; debian.mkdir(exist_ok=True)
        (debian / 'control').write_text(f'Source: keepassxc-gnome\nSection: utils\nPriority: optional\nMaintainer: {args.maintainer}\n\nPackage: {name}\nArchitecture: any\nDescription: GTK frontend for KeePassXC\n')
        shlibs = debian / 'shlibs.local'; shlibs.write_text(''.join(local_shlibs))
        command = ['dpkg-shlibdeps', '-O', '-L' + str(shlibs)]
        library = cache / 'sysroot/usr/lib' / multiarch
        if library.exists(): command += ['-l' + str(library)]
        command += ['-e' + path.relative_to(work).as_posix() for path in executables]
        calculated = run(command, cwd=work)
        dependencies = next(line.split('=', 1)[1] for line in calculated.splitlines() if line.startswith('shlibs:Depends='))
        dependencies += ', gsettings-desktop-schemas, xdg-desktop-portal, xdg-desktop-portal-gnome'
        manifests = {
            'schema': 1, 'package': name, 'version': args.version, 'architecture': architecture,
            'development_ui': development_ui, 'release_package': args.release,
            'source_date_epoch': epoch, 'upstream': pin, 'depends': dependencies,
            'source': {'file': source.name, 'sha256': sha(source)},
            'patches': {p.name: sha(p) for p in sorted((ROOT / 'patches').glob('*.patch'))},
            'development_packages': packages,
            'os_release': Path('/etc/os-release').read_text(),
            'pkg_config': {name: run(['pkg-config', '--modversion', name]) for name in ['gtk4', 'libadwaita-1', 'glib-2.0', 'pango', 'pangoft2', 'cairo', 'fontconfig', 'Qt6Core', 'botan-3']},
            'toolchain': {tool: run([tool, '--version']).splitlines()[0] for tool in ['cmake', 'c++', 'ninja', 'dpkg-deb', 'strip']},
            'binaries': {p.name: sha(p) for p in executables},
            'qualification': 'Ubuntu 26.04 GNOME/Wayland; short personal sessions; see README limitations' if args.release else 'development-only; VM install and physical-session checks required',
        }
        manifest = json.dumps(manifests, indent=2, sort_keys=True) + '\n'
        (doc / 'build-manifest.json').write_text(manifest)
        (output / f'{name}_{args.version}_{architecture}.manifest.json').write_text(manifest)
        size = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file() and not p.is_symlink()) // 1024
        transition = 'Conflicts: keepassxc-gnome-development\nReplaces: keepassxc-gnome-development\n' if args.release else ''
        description = ('Password manager for GNOME using the KeePassXC core\n GTK4/libadwaita interface for encrypted KDBX databases.\n Supports Ubuntu 26.04 GNOME/Wayland for short personal sessions.' if args.release else 'Development GTK4/libadwaita frontend for KeePassXC\n Synthetic test databases only; not qualified for real credentials.')
        (control / 'control').write_text(f'Package: {name}\nVersion: {args.version}\nSection: utils\nPriority: optional\nArchitecture: {architecture}\nMaintainer: {args.maintainer}\nHomepage: https://github.com/NBE03xxx/keepassxc-gnome\nInstalled-Size: {size}\nDepends: {dependencies}\n{transition}Description: {description}\n')
        for path in stage.rglob('*'):
            if path.is_symlink(): continue
            path.chmod(0o755 if path.is_dir() or path in executables else 0o644)
        md5 = []
        for path in sorted(stage.rglob('*')):
            if path.is_file() and not path.is_symlink() and control not in path.parents:
                md5.append(hashlib.md5(path.read_bytes()).hexdigest() + '  ' + path.relative_to(stage).as_posix())
        (control / 'md5sums').write_text('\n'.join(md5) + '\n')
        for path in sorted(stage.rglob('*'), reverse=True): os.utime(path, (epoch, epoch), follow_symlinks=False)
        os.utime(stage, (epoch, epoch))
        artifact = output / f'{name}_{args.version}_{architecture}.deb'
        subprocess.run(['dpkg-deb', '--root-owner-group', '-Zxz', '-z6', '--build', str(stage), str(artifact)], check=True)
    artifacts = [source, artifact, output / f'{name}_{args.version}_{architecture}.manifest.json']
    (output / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + p.name + '\n' for p in artifacts))
    print(artifact)

if __name__ == '__main__': main()
