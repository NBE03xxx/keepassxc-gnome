#!/usr/bin/env python3
"""Inspect/exercise a staged deb, without installing it or running maintainer scripts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run(args, **kwargs): return subprocess.check_output([str(a) for a in args], text=True, **kwargs).strip()
def require(ok, label):
    if not ok: raise AssertionError(label)
    print('PASS', label, flush=True)

parser = argparse.ArgumentParser(); parser.add_argument('deb', type=Path); args = parser.parse_args()
deb = args.deb.resolve()
package = run(['dpkg-deb','-f',deb,'Package'])
require(package in ('keepassxc-gnome-development', 'keepassxc-gnome'), 'independent package name')
release = package == 'keepassxc-gnome'
with tempfile.TemporaryDirectory(prefix='package-test-', dir=ROOT / 'build') as temp:
    work = Path(temp); stage = work / 'stage'; stage.mkdir()
    foreign = work / 'foreign-build'; foreign.mkdir()
    (foreign / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=/different-project\nKEEPASSXC_SOURCE_DIR:PATH=/different-upstream\n')
    require(subprocess.run(['python3',str(ROOT/'tools/build-deb.py'),'--build',str(foreign),'--output',str(work/'refused')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode != 0, 'foreign or candidate build cannot be packaged as the fixed baseline')
    subprocess.run(['dpkg-deb','--extract',str(deb),str(stage)],check=True)
    control = work / 'control'; subprocess.run(['dpkg-deb','--control',str(deb),str(control)],check=True)
    require(set(p.name for p in control.iterdir()) == {'control','md5sums'}, 'no maintainer scripts or conffiles')
    metadata = (control / 'control').read_text()
    relationships = [line for line in metadata.splitlines() if re.match(r'^(Conflicts|Replaces|Provides):', line)]
    require(relationships == (['Conflicts: keepassxc-gnome-development', 'Replaces: keepassxc-gnome-development'] if release else []), 'only own development package may be replaced; never original KeePassXC')
    require('libbotan-3-10' in metadata and 'xdg-desktop-portal-gnome' in metadata, 'crypto and native picker runtime dependencies')
    application = stage / 'usr/libexec/keepassxc-gnome'; binary = stage / 'usr/bin/keepassxc-gnome'
    require(binary.is_symlink() and binary.resolve() == application / 'keepassxc-gnome', 'private helpers reached through relative launcher')
    doc = stage / 'usr/share/doc' / package
    manifest = json.loads((doc / 'build-manifest.json').read_text())
    require('keepassxc-gnome-browser-host' in manifest['binaries'], 'bundled Native Messaging host identity')
    require('keepassxc-gnome-auto-type' in manifest['binaries'], 'bundled Auto-Type helper identity')
    ime_extension = stage / 'usr/share/gnome-shell/extensions/keepassxc-auto-type-ime@keepassxc-gnome'
    require((ime_extension / 'extension.js').is_file()
            and json.loads((ime_extension / 'metadata.json').read_text())['uuid'] == ime_extension.name
            and not (stage / 'usr/share/glib-2.0/schemas').exists(),
            'bundled IME extension with user-controlled activation')
    extension_manifest = manifest['gnome_shell_extension']
    require(extension_manifest['uuid'] == ime_extension.name
            and set(extension_manifest['files']) == {'metadata.json', 'extension.js'}
            and all(hashlib.sha256((ime_extension / name).read_bytes()).hexdigest() == digest
                    for name, digest in extension_manifest['files'].items()), 'bundled IME extension identity')
    require(not any(p.name == 'org.keepassxc.keepassxc_browser.json' for p in stage.rglob('*')),
            'Chrome registration is created by explicit user action only')
    require(hashlib.sha256((deb.parent / manifest['source']['file']).read_bytes()).hexdigest() == manifest['source']['sha256'], 'matching complete corresponding source')
    for name, digest in manifest['binaries'].items():
        path = application / name
        require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, name + ' identity')
        dynamic = run(['readelf','-d',path]); program = run(['readelf','-lW',path]); header = run(['readelf','-h',path])
        require('RPATH' not in dynamic and 'RUNPATH' not in dynamic and 'libasan' not in dynamic, name + ' uninstrumented / no build RPATH')
        stack = next(line for line in program.splitlines() if 'GNU_STACK' in line)
        require('RWE' not in stack and 'GNU_RELRO' in program and 'BIND_NOW' in dynamic and re.search(r'Type:\s+DYN', header), name + ' PIE / RELRO / NOW / non-executable stack')
        require('libQt6Widgets' not in dynamic, name + ' does not link Qt Widgets')
    for path in stage.rglob('*'):
        if path.is_file() and not path.is_symlink(): require(not path.stat().st_mode & 0o6000, path.relative_to(stage).as_posix() + ' no set-ID')
    version_text = run([binary,'--version'])
    package_version = run(['dpkg-deb', '-f', deb, 'Version'])
    require(re.fullmatch(r'KeePassXC GNOME(?: Development)? [0-9]+\.[0-9]+\.[0-9]+', version_text)
            and (not release or version_text == 'KeePassXC GNOME ' + package_version.rsplit('-', 1)[0]),
            'staged executable version and public branding')
    require(manifest.get('release_package', False) == release and (not release or manifest.get('development_ui') is False), 'release package cannot contain development UI')
    require(run([binary,'--language-check'],env=dict(os.environ, LC_ALL='ja_JP.UTF-8',LANGUAGE='ja')) == '準備完了', 'staged Japanese translation path')
    require(run([binary,'--language-check'],env=dict(os.environ, LC_ALL='C.UTF-8',LANGUAGE='de')) == 'Ready', 'staged English fallback')
    desktop = stage / 'usr/share/applications/org.keepassxc.Gnome.Development.desktop'
    subprocess.run(['desktop-file-validate',str(desktop)],check=True)
    require('Name=KeePassXC GNOME\n' in desktop.read_text() and 'Name[ja]=KeePassXC GNOME\n' in desktop.read_text(), 'launcher has public Japanese and English names')
    require('MimeType=application/x-keepass2;' in desktop.read_text() and 'Exec=keepassxc-gnome %f' in desktop.read_text(), 'optional KDBX handler with supported single-file argument')
    require((stage/'usr/share/mime/packages/org.keepassxc.Gnome.Development.xml').is_file(), 'independent KDBX MIME definition')
    require(not any(p.name == 'mimeapps.list' for p in stage.rglob('*')), 'no installed default-application override')
    for name in ['LICENSE.GPL-2','LICENSE.GPL-3','upstream-COPYING','upstream-LICENSE.LGPL-2.1','upstream-LICENSE.NOKIA-LGPL-EXCEPTION','upstream-LICENSE.Apache-2.0']:
        require((doc / name).stat().st_size > 0, name + ' attribution present')
    profile = work / 'profile'
    environment = dict(os.environ, XDG_CONFIG_HOME=str(profile/'config'), XDG_CACHE_HOME=str(profile/'cache'), XDG_STATE_HOME=str(profile/'state'))
    require(subprocess.run([str(ROOT/'build/bin/gnome_process_domain_tests'),'unused',str(work/'missing-worker')],env=environment,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode != 0, 'missing explicit worker does not silently fall back to development worker')
    subprocess.run([str(ROOT/'build/bin/gnome_process_domain_tests'),str(ROOT/'build/reference/gnome_reference_fixture'),str(application/'keepassxc-gnome-worker')],env=environment,check=True)
    require(True, 'installed-layout worker: synthetic create / read / edit / save / lock')
    subprocess.run([str(ROOT/'build/bin/gnome_picker_tests'),str(application/'keepassxc-gnome-picker')],env=environment,check=True)
    require(True, 'installed-layout picker: selection / cancellation / bad replies / reaping')
    source = work / 'source'; source.mkdir()
    with tarfile.open(deb.parent / manifest['source']['file']) as archive:
        archive.extractall(source, filter='data')
    root = next(source.iterdir()); upstream = root / 'upstream/keepassxc'
    require(not (upstream / '.git').exists(), 'corresponding source carries no local Git metadata')
    subprocess.run(['python3',str(root/'tools/verify-upstream.py'),str(upstream)],check=True)
    victim = upstream / 'README.md'; original = victim.read_bytes(); victim.write_bytes(original + b'\nmodified\n')
    refused = subprocess.run(['python3',str(root/'tools/verify-upstream.py'),str(upstream)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode != 0
    victim.write_bytes(original); require(refused,'modified upstream archive refused')
    extra = upstream / 'unexpected'; extra.write_text('synthetic test');
    require(subprocess.run(['python3',str(root/'tools/verify-upstream.py'),str(upstream)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode != 0,'extra upstream archive files refused'); extra.unlink()
    subprocess.run(['cmake','-S',str(root),'-B',str(work/'source-build'),'-G','Ninja'],check=True,stdout=subprocess.DEVNULL)
    require(True, 'Git-free corresponding source configures')
print('PASS package contents and installed-layout smoke tests; no host installation')
