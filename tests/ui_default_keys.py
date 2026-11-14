"""Real Return/KP_Enter in product dialogs. Private Wayland and synthetic data only."""
import os
import time
from pathlib import Path
import gi
gi.require_version('Atspi','2.0')
from gi.repository import Atspi,GLib
from private_keyboard import PrivateKeyboard

pid=int(os.environ['KPXC_UI_PID']);root=Path(os.environ['KPXC_UI_ROOT']);english=os.environ.get('KPXC_UI_LANGUAGE')=='en'
keyboard=PrivateKeyboard();key=0xff8d if english else 0xff0d

def walk(node,depth=0):
    if not node or depth>40:return
    node.clear_cache();yield node
    for i in range(node.get_child_count()):yield from walk(node.get_child_at_index(i),depth+1)
def app():return next((a for a in Atspi.get_desktop(0) if a and a.get_process_id()==pid),None)
def wait(test,label,seconds=25):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        while GLib.MainContext.default().pending():GLib.MainContext.default().iteration(False)
        log=(root/'app.log').read_text(errors='replace')
        assert 'FAIL default keys UI:' not in log,log
        try:
            result=test()
            if result:return result
        except GLib.Error:pass
        time.sleep(.05)
    raise AssertionError(label+'\n'+(root/'app.log').read_text(errors='replace')[-3000:])
def phase(name):
    wait(lambda:(root/'default-keys-phase').exists() and (root/'default-keys-phase').read_text()==name,'phase '+name)
    wait(app,'test application registered')
    for _ in range(20):
        try:
            keyboard.activate_window(pid)
            break
        except AssertionError:time.sleep(.1)
    else:raise AssertionError('private test window ready')
def dialog():return next((n for n in walk(app()) if n.get_role_name() in ('alert','dialog')),None)
def fields():
    d=dialog()
    return [n for n in walk(d) if n.get_role_name() in ('text','password text') and n.get_editable_text_iface() and not n.get_state_set().contains(Atspi.StateType.MULTI_LINE)] if d else []
def text(field,value):assert field.get_editable_text_iface().set_text_contents(value)
def focus(field):
    # GTK's protected text delegate need not implement AT-SPI GrabFocus.
    # Move through the real keyboard focus chain instead.
    for _ in range(45):
        field.clear_cache()
        if field.get_state_set().contains(Atspi.StateType.FOCUSED):return
        in_notes=any(n.get_state_set().contains(Atspi.StateType.FOCUSED) and (n.get_state_set().contains(Atspi.StateType.MULTI_LINE) or (n.get_role_name()=='text' and n.get_name()==('Notes' if english else 'メモ'))) for n in walk(dialog()))
        if in_notes:keyboard.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe3,True)))
        keyboard.key(0xff09)
        if in_notes:keyboard.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe3,False)))
    raise AssertionError('keyboard focus target '+field.get_name())

def enter(field):focus(field);keyboard.key(key)
def named_button(ja,en):
    label=en if english else ja
    for n in walk(dialog()):
        if n.get_name().strip()==label:
            for c in walk(n):
                action=c.get_action_iface()
                if action and action.get_n_actions():return c
    return None

try:
    phase('create');f=wait(lambda:fields() if len(fields())==3 else None,'create fields')
    text(f[0],'Keyboard DB');text(f[1],'keyboard-master');text(f[2],'mismatch');enter(f[2]);time.sleep(.2)
    assert dialog() and len(fields())==3,'mismatch Enter keeps credentials open'
    text(f[2],'keyboard-master');text(f[0],'');enter(f[0]);time.sleep(.2)
    assert dialog() and len(fields())==3,'empty name Enter stays in form'
    text(f[0],'Keyboard DB');enter(f[2])
    phase('new-entry');f=wait(lambda:fields() if len(fields())>=4 else None,'entry fields')
    enter(f[0]);time.sleep(.2);assert dialog(),'empty title does not dismiss editor'
    for field,value in zip(f,['Keyboard entry','keyboard-user','keyboard-initial-secret','https://keyboard.example.invalid']):text(field,value)
    enter(f[0])
    phase('edit-username');f=fields();text(f[1],'second-user');enter(f[1])
    phase('edit-password')
    generate=wait(lambda:named_button('生成','Generate'),'Generate button');focus(generate);keyboard.key(key);time.sleep(.2)
    assert dialog(),'Enter on focused Generate does not save the entry'
    f=fields();text(f[2],'keyboard-changed-secret')
    assert f[2].get_role_name()=='password text','password remains protected'
    enter(f[2])
    phase('edit-url');f=fields();text(f[3],'https://keyboard.example.invalid/edited');enter(f[3])
    phase('new-group');f=fields();enter(f[0]);time.sleep(.2);assert dialog(),'empty group Enter stays open'
    text(f[0],'Keyboard group');enter(f[0])
    phase('edit-group');f=fields();text(f[0],'Keyboard group renamed');enter(f[0])
    phase('notes')
    # The test probe opens Notes, which is normally collapsed. GTK versions
    # need not expose MULTI_LINE; use the explicit accessible field label.
    def multiline():return next((n for n in walk(dialog()) if n.get_role_name()=='text' and n.get_name()==('Notes' if english else 'メモ')),None)
    note=wait(multiline,'visible notes');text(note,'first line');focus(note)
    keyboard.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe3,True)));keyboard.key(0xff57);keyboard.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe3,False)))
    keyboard.key(key);time.sleep(.2)
    assert dialog() and Atspi.Text.get_text(note,0,-1)=='first line\n','notes Enter inserts a newline without saving'
    save=wait(lambda:named_button('エントリーを保存','Save entry'),'Save entry button');enter(save)
    phase('edit-before-close');f=fields();text(f[0],'Keyboard final');enter(f[0])
    phase('close-save');wait(lambda:named_button('保存（上書き）','Save (overwrite)'),'default close-save response');keyboard.key(key)
    wait(lambda:'PASS default keys GUI:' in (root/'app.log').read_text(errors='replace'),'durable save and closed application',seconds=30)
    print('PASS actual '+('KP_Enter' if english else 'Return')+': validated creation, title/username/masked password/URL saves, group create/edit, auxiliary button focus, notes newline and close-save',flush=True)
finally:keyboard.close()
