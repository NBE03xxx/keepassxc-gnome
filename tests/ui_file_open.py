"""Desktop file handoff and MIME preferences, disposable GNOME/XDG only.
No original app or real database is launched, read, configured or modified.
"""
import hashlib
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import tempfile
import gi
gi.require_version('Atspi', '2.0')
gi.require_version('Gio', '2.0')
from gi.repository import Atspi, Gio, GLib

root = Path(os.environ['KPXC_UI_ROOT']).resolve()
assert root in Path(os.environ['XDG_RUNTIME_DIR']).resolve().parents
assert os.environ['GSETTINGS_BACKEND'] == 'memory'
assert os.environ['DBUS_SYSTEM_BUS_ADDRESS'] == os.environ['DBUS_SESSION_BUS_ADDRESS']
product = Path(os.environ['KPXC_UI_APP']).resolve()
assert re.fullmatch(r'[A-Za-z0-9_./-]+', str(product)), 'safe desktop command path'
english = os.environ.get('KPXC_UI_LANGUAGE') == 'en'
initial_pid = int(os.environ['KPXC_UI_PID'])
owned = set()
processes = []

def text(ja, en): return en if english else ja

def walk(node, depth=0):
    if not node or depth > 40: return
    node.clear_cache()
    yield node
    for i in range(node.get_child_count()): yield from walk(node.get_child_at_index(i), depth + 1)

def apps():
    return [a for a in Atspi.get_desktop(0) if a and a.get_process_id() in owned]

def nodes(pid):
    app = next((a for a in apps() if a.get_process_id() == pid), None)
    return list(walk(app)) if app else []

def named(pid, label, role=None):
    return next((n for n in nodes(pid) if n.get_name().strip() == label and (not role or role in n.get_role_name())), None)

def wait(test, label, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            while GLib.MainContext.default().pending(): GLib.MainContext.default().iteration(False)
            value = test()
            if value: return value
        except (GLib.Error, FileNotFoundError): pass
        time.sleep(.08)
    raise AssertionError(label)

def click(pid, label):
    def ready():
        for n in nodes(pid):
            if n.get_name().strip() != label or 'button' not in n.get_role_name(): continue
            if not n.get_state_set().contains(Atspi.StateType.SENSITIVE): continue
            for c in walk(n):
                a = c.get_action_iface()
                if a and a.get_n_actions(): return a
    assert wait(ready, 'button ' + label).do_action(0)


def close(pid):
    # Only PIDs returned by this driver's GAppLaunchContext/Popen. The
    # hardened product intentionally makes /proc/PID/exe unreadable.
    assert pid in owned  # PID from our GAppLaunchContext or our Popen only
    os.kill(pid, signal.SIGTERM)
    wait(lambda: not any(a.get_process_id() == pid for a in apps()), 'owned test UI closed')
    owned.remove(pid)

try:
    data = Path(os.environ['XDG_DATA_HOME']); config = Path(os.environ['XDG_CONFIG_HOME'])
    applications = data/'applications'; applications.mkdir(parents=True, exist_ok=True)
    original_id = 'org.example.KeePassXC.TestStub.desktop'
    stub = applications/original_id
    stub.write_text('[Desktop Entry]\nType=Application\nName=KeePassXC (test stub)\nExec=/usr/bin/false %f\nMimeType=application/x-keepass2;\n')
    original = Gio.DesktopAppInfo.new_from_filename(str(stub))
    assert original and original.set_as_default_for_type('application/x-keepass2')
    preferences = config/'mimeapps.list'; before = preferences.read_bytes()
    mime = data/'mime'; (mime/'packages').mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[1]
    (mime/'packages/org.keepassxc.Gnome.Development.xml').write_bytes((source/'packaging/org.keepassxc.Gnome.Development.xml').read_bytes())
    subprocess.run(['update-mime-database', str(mime)], check=True)
    desktop = applications/'org.keepassxc.Gnome.Development.desktop'
    content = (source/'packaging/org.keepassxc.Gnome.Development.desktop').read_text()
    content = content.replace('Exec=keepassxc-gnome %f', f'Exec="{product}" %f') + f'Path={root}\n'
    desktop.write_text(content)
    subprocess.run(['desktop-file-validate', str(desktop)], check=True)
    subprocess.run(['update-desktop-database', str(applications)], check=True)
    assert preferences.read_bytes() == before, 'registering candidate never changes explicit default'
    assert Gio.AppInfo.get_default_for_type('application/x-keepass2', False).get_id() == original_id
    candidate = Gio.DesktopAppInfo.new_from_filename(str(desktop)); assert candidate
    handlers = {a.get_id() for a in Gio.AppInfo.get_all_for_type('application/x-keepass2')}
    assert {original_id, desktop.name} <= handlers, handlers
    fixture = Path(tempfile.mkdtemp(prefix="handoff-", dir=root))/'合成 & \' " $(touch SHOULD_NOT_EXIST) <db>.kdbx'
    subprocess.run([os.environ['KPXC_UI_FIXTURE_WRITER'], '--write-fixture', str(fixture)], check=True)
    assert subprocess.check_output(['xdg-mime', 'query', 'filetype', str(fixture)], text=True).strip() == 'application/x-keepass2'
    original_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    launched = []
    context = Gio.AppLaunchContext()
    context.connect('launched', lambda _ctx, _info, data: launched.append(data.unpack()['pid']))
    assert candidate.launch_uris([Gio.File.new_for_path(str(fixture)).get_uri()], context)
    pid = wait(lambda: launched[0] if launched else None, 'GIO reports the process it launched')
    assert isinstance(pid, int) and pid > 1
    owned.add(pid)
    password = wait(lambda: next((n for n in nodes(pid) if n.get_role_name() == 'password text'), None), 'local file reaches ordinary credential dialog after guard initialization')
    assert named(pid, fixture.name, 'label'), 'handoff preserves UTF-8, quotes and spaces without shell evaluation'
    assert not (root/'SHOULD_NOT_EXIST').exists(), 'file argument is never executed'
    assert hashlib.sha256(fixture.read_bytes()).hexdigest() == original_hash
    assert not (Path(os.environ['XDG_STATE_HOME'])/'keepassxc-gnome/recent.ini').exists(), 'file selection alone does not record history'
    assert password.get_editable_text_iface().set_text_contents('phase2-demo')
    click(pid, text('ロック解除', 'Unlock'))
    wait(lambda: named(pid, fixture.name, 'label') and not any(n.get_role_name() == 'alert' for n in nodes(pid)), 'real worker unlock succeeds')
    click(pid, text('ロック', 'Lock'))
    wait(lambda: named(pid, text('ロックを解除してください', 'Unlock your database'), 'label'), 'locked recent-file screen')
    close(pid)
    assert hashlib.sha256(fixture.read_bytes()).hexdigest() == original_hash, 'opening and locking never changes DB'
    assert preferences.read_bytes() == before, 'opening app never changes explicit default'
    for arguments, label in [(['https://example.invalid/not-opened.kdbx'], text('ローカルのデータベースファイルだけを開けます。ローカルのKDBXファイルを選択してください。', 'Only local database files can be opened. Choose a local KDBX file.')),
                             ([str(fixture), str(fixture)], text('データベースは1件ずつ開いてください。', 'Open one database at a time.'))]:
        p = subprocess.Popen([str(product), *arguments], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); processes.append(p); owned.add(p.pid)
        wait(lambda: named(p.pid, label, 'label'), 'unsupported handoff explains rejection')
        assert not any(n.get_role_name() == 'password text' for n in nodes(p.pid)), 'rejected request has no credential form'
        close(p.pid); p.wait(timeout=5)
    print('PASS file association GUI: both handlers offered, explicit original default unchanged, desktop %f handoff, UTF-8/quotes/spaces, fresh-password unlock, lock, URI/multiple-file rejection, no DB change', flush=True)
finally:
    for pid in owned:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError: pass
    for p in processes:
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
