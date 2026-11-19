"""Actual Return/KP_Enter in synthetic combined-factor and key-only forms."""
import os
import time
from pathlib import Path
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi
from private_keyboard import PrivateKeyboard

root = Path(os.environ['KPXC_UI_ROOT'])
pid = int(os.environ['KPXC_UI_PID'])
keyboard = PrivateKeyboard()
key = 0xff8d if os.environ.get('KPXC_UI_LANGUAGE') == 'en' else 0xff0d

def wait(predicate, label):
    end = time.monotonic() + 35
    while time.monotonic() < end:
        log = (root / 'app.log').read_text(errors='replace')
        assert 'FAIL key file' not in log, log
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError(label + '\n' + (root / 'app.log').read_text(errors='replace'))

for phase in ['combined', 'key-only']:
    wait(lambda: (root / 'keyfile-phase').exists() and (root / 'keyfile-phase').read_text() == phase, phase)
    keyboard.activate_window(pid)
    time.sleep(.15)
    def walk(node, depth=0):
        if not node or depth > 40:
            return
        node.clear_cache()
        yield node
        for index in range(node.get_child_count()):
            yield from walk(node.get_child_at_index(index), depth + 1)
    app = next(app for app in Atspi.get_desktop(0) if app and app.get_process_id() == pid)
    english = os.environ.get('KPXC_UI_LANGUAGE') == 'en'
    for _ in range(45):
        dialog = next((n for n in walk(app) if n.get_role_name() in ('alert', 'dialog')), None)
        nodes = list(walk(dialog))
        if phase == 'combined':
            target = next((n for n in nodes if n.get_role_name() == 'password text'), None)
        else:
            target = next((n for n in nodes if n.get_name().strip() == ('Unlock' if english else 'ロック解除') and n.get_action_iface() and n.get_action_iface().get_n_actions()), None)
        if target and target.get_state_set().contains(Atspi.StateType.FOCUSED):
            break
        keyboard.key(0xff09)
        time.sleep(.03)
    else:
        raise AssertionError('real keyboard focus for ' + phase)
    keyboard.key(key)
wait(lambda: 'PASS key file GUI:' in (root / 'app.log').read_text(errors='replace'), 'completed key-file workflow')
print('PASS actual key-file ' + ('KP_Enter' if key == 0xff8d else 'Return') + ': combined and key-only authentication')
