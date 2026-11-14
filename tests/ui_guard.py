"""Check the real product's authentication gate with an unreachable system bus."""
import os
from pathlib import Path
import time
import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi

pid = int(os.environ['KPXC_UI_PID'])
japanese = os.environ.get('LANGUAGE', 'ja').startswith('ja')
names = ('データベースを作成', 'データベースを開く') if japanese else ('New database', 'Open database')
warning = ('一部のシステムロック通知を受信できません。5分間操作しない場合のロックは有効です。'
           if japanese else 'Some system lock notifications are unavailable. The five-minute inactivity lock remains enabled.')


def walk(node, depth=0):
    if depth > 40:
        return
    yield node
    for index in range(node.get_child_count()):
        child = node.get_child_at_index(index)
        if child:
            yield from walk(child, depth + 1)


def application():
    desktop = Atspi.get_desktop(0)
    for index in range(desktop.get_child_count()):
        child = desktop.get_child_at_index(index)
        if child and child.get_process_id() == pid:
            return child


def state():
    app = application()
    if not app:
        return None
    nodes = list(walk(app))
    buttons = [node for node in nodes if 'button' in node.get_role_name() and node.get_name().strip() in names]
    if not all(any(node.get_name().strip() == name for node in buttons) for name in names):
        return None
    if not any(node.get_name().strip() == warning and node.get_state_set().contains(Atspi.StateType.SHOWING) for node in nodes):
        return None
    assert all(not node.get_state_set().contains(Atspi.StateType.SENSITIVE) for node in buttons), 'authentication controls must stay disabled after bus failure'
    assert not any(node.get_role_name() == 'password text' for node in nodes), 'no authentication form'
    for child in (Path('/proc') / str(pid) / 'task' / str(pid) / 'children').read_text().split():
        assert (Path('/proc') / child / 'exe').resolve().name != 'keepassxc-gnome-worker', 'no database worker'
    return buttons


deadline = time.monotonic() + 15
buttons = None
while time.monotonic() < deadline and buttons is None:
    buttons = state()
    time.sleep(.1)
if buttons is None:
    app = application()
    print('Expected product PID:', pid, 'registered:', bool(app), flush=True)
    if app:
        for node in walk(app):
            print(node.get_role_name(), repr(node.get_name()), node.get_state_set().get_states(), flush=True)
assert buttons is not None, 'application, authentication controls and monitoring warning appeared'
# Accessibility activation must not bypass the same gate as normal clicks.
for button in buttons:
    action = button.get_action_iface()
    if action:
        action.do_action(0)
deadline = time.monotonic() + 3
while time.monotonic() < deadline:
    assert state() is not None, 'application remains locked and usable after attempted activation'
    time.sleep(.1)
print('PASS guard GUI: unreachable system bus, visible warning, disabled create/open, no authentication form or database worker, accessibility activation gated')
