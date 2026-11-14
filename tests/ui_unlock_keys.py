"""Actual Return / keypad Enter on the real credential dialog, private GNOME only.

The lifetime probe prepares an upstream-encrypted recovery fixture and waits
at the ordinary product dialog. No production authentication hook is added.
"""
import os
import time
from pathlib import Path
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib
from private_keyboard import PrivateKeyboard

pid = int(os.environ['KPXC_UI_PID'])
root = Path(os.environ['KPXC_UI_ROOT'])
english = os.environ.get('KPXC_UI_LANGUAGE') == 'en'
keyboard = PrivateKeyboard()

def walk(node, depth=0):
    if node is None or depth > 35:
        return
    node.clear_cache()
    yield node
    for i in range(node.get_child_count()):
        yield from walk(node.get_child_at_index(i), depth + 1)

def app():
    return next((a for a in Atspi.get_desktop(0) if a and a.get_process_id() == pid), None)

def wait(test, label, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        while GLib.MainContext.default().pending():
            GLib.MainContext.default().iteration(False)
        try:
            result = test()
        except GLib.Error:
            result = None
        if result:
            return result
        time.sleep(.05)
    raise AssertionError(label)

def password_field():
    title = 'Unlock database' if english else 'データベースのロック解除'
    dialog = next((n for n in walk(app()) if n.get_role_name() == 'alert' and n.get_name() == title), None)
    if dialog:
        return next((n for n in walk(dialog) if n.get_role_name() == 'password text'), None)

field = wait(password_field, 'ordinary recovery credential dialog')
keyboard.activate_window(pid)
assert field.get_editable_text_iface().set_text_contents('phase2-demo')
for _ in range(25):
    field.clear_cache()
    if field.get_state_set().contains(Atspi.StateType.FOCUSED):
        break
    keyboard.key(0xff09)
else:
    raise AssertionError('password field reachable by Tab')
key = 0xff8d if english else 0xff0d
keyboard.key(key)
wait(lambda: not password_field(), 'Enter dismisses credential dialog')
wait(lambda: 'PASS lifetime GUI:' in (root / 'app.log').read_text(errors='replace'), 'Enter unlocks and completes the protected recovery workflow')
print('PASS actual ' + ('keypad Enter' if english else 'Return') + ': password field unlocks through the normal protected response', flush=True)
keyboard.close()
