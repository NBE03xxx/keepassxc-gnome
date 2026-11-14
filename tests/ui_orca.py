#!/usr/bin/env python3
"""Drive only the synthetic app on a private AT-SPI bus with real Orca running."""
import os
import re
import json
import gettext
from pathlib import Path
import time
import gi
gi.require_version('Atspi','2.0')
from gi.repository import Atspi,GLib
root=Path(os.environ['KPXC_UI_ROOT']);pid=int(os.environ['KPXC_UI_PID'])
english=os.environ.get('KPXC_UI_LANGUAGE')=='en'
translations={'データベースを作成':'New database','作成':'Create','エントリーを作成':'New entry','エントリーを保存':'Save entry','エントリーを検索':'Search entries','ロック':'Lock','未保存の変更を破棄しますか？':'Discard unsaved changes?','破棄':'Discard','データベースはロックされています':'Your database is locked','閉じる':'Close','データベース名':'Database name','マスターパスワード':'Master password','マスターパスワード（確認）':'Repeat master password','タイトル':'Title','ユーザー名':'Username','エントリーを編集':'Edit entry','パスワードのプレビュー':'Password preview','エントリー一覧':'Entries'}
def ui(text):return translations.get(text,text) if english else text
speech_log=root/'orca-debug.log'
from private_keyboard import PrivateKeyboard
keyboard=PrivateKeyboard()
checks=0
echo_cases=[]

def walk(node,depth=0):
    if node is None or depth>40:return
    node.clear_cache();yield node
    for i in range(node.get_child_count()):
        child=node.get_child_at_index(i)
        if child is not None:yield from walk(child,depth+1)
def app():
    for child in Atspi.get_desktop(0):
        if child is not None and child.get_process_id()==pid:
            child.set_cache_mask(Atspi.Cache.NONE);return child

def pump():
    for _ in range(30):
        if not GLib.MainContext.default().iteration(False):break

def wait(fn,label,timeout=20):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        pump()
        try:
            value=fn()
            if value:return value
        except Exception:pass
        time.sleep(.05)
    raise AssertionError(label)

def named(name,role=None):
    name=ui(name)
    return next((n for n in walk(app()) if n.get_name().strip()==name and (role is None or role in n.get_role_name()) and n.get_state_set().contains(Atspi.StateType.SHOWING)),None)
def click(name):
    n=wait(lambda:named(name,'button'),'button '+name)
    wait(lambda:n.get_state_set().contains(Atspi.StateType.SENSITIVE),'sensitive '+name)
    focus(n,'button '+name)
    keyboard.key(0xff0d)
def fields(dialog):return [n for n in walk(dialog) if n.get_role_name() in ('text','password text') and n.get_editable_text_iface()]
def speech():
    if not speech_log.exists():return ''
    return '\n'.join(line for line in speech_log.read_text(errors='replace').splitlines() if 'SPEECH OUTPUT:' in line)
def focus(node,label):
    # GTK4 need not implement AT-SPI Component.GrabFocus. Use actual Tab
    # events on the private compositor, and verify the focused accessible.
    for _ in range(45):
        node.clear_cache()
        if node.get_state_set().contains(Atspi.StateType.FOCUSED) or (label=='entry list' and any(n.get_state_set().contains(Atspi.StateType.FOCUSED) for n in walk(node))):
            time.sleep(.3);pump();return
        keyboard.key(0xff09);pump()
        # Focus is checked against the live accessible on the next iteration.
    raise AssertionError(label+' reachable with Tab')

def check(ok,label):
    global checks
    assert ok,label
    checks+=1;print('PASS',label,flush=True)

def echo_case(label,node,protected):
    # An absent utterance alone is not a pass: prove actual keyboard delivery
    # and, for protected fields, Orca's processing of all three key presses.
    assert node.get_editable_text_iface().set_text_contents('')
    focus(node,label)
    node.clear_cache()
    check((node.get_role_name()=='password text')==protected,label+' role matches visibility')
    start=len(speech());debug_start=len(speech_log.read_text())
    for letter in 'qzv':keyboard.key(ord(letter))
    wait(lambda:Atspi.Text.get_character_count(node)==3,label+' actual typed character count')
    if protected:
        wait(lambda:speech_log.read_text()[debug_start:].count('Echoing password text keyboard event: False')>=3,label+' Orca processed three protected key presses')
    else:
        wait(lambda:all(re.search(r"SPEECH OUTPUT: '"+letter+r"'",speech()[start:],re.I) for letter in 'qzv'),label+' positive key echo control')
    time.sleep(.6);pump();node.clear_cache()
    accessible=Atspi.Text.get_text(node,0,-1)
    spoken=[letter for letter in 'qzv' if re.search(r"SPEECH OUTPUT: '"+letter+r"'",speech()[start:],re.I)]
    check(len(accessible)==3 and (not any(letter in accessible.lower() for letter in 'qzv') if protected else accessible=='qzv'),label+' accessible text matches masking')
    check(not spoken if protected else len(spoken)==3,label+' speech matches visibility')
    echo_cases.append({'case':label,'role':node.get_role_name(),'typed_character_count':3,'accessible_masked':protected,'letters_spoken':spoken})
    (root/'echo-cases.json').write_text(json.dumps(echo_cases,indent=2,ensure_ascii=False)+'\n')

def toggle_visibility(node,protected_after):
    # The visible value is deliberately public synthetic data, never a canary.
    assert node.get_editable_text_iface().set_text_contents('qzv')
    focus(node,'password visibility context menu')
    label='_Show Text' if english else gettext.translation('gtk40',languages=['ja'],fallback=True).gettext('_Show Text')
    label=label.replace('_','').split('(')[0].strip()
    start=len(speech())
    keyboard.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe1,True)))
    keyboard.key(0xffc7)  # Shift+F10 on the private compositor.
    keyboard.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe1,False)))
    time.sleep(.4);pump()
    # GTK popover items need not be descendants of the application's root
    # accessible. Navigate by keyboard and verify real Orca's current label.
    for _ in range(30):
        if label in speech()[start:]:break
        start=len(speech());keyboard.key(0xff54)
        wait(lambda:len(speech())>start,'Orca context menu navigation')
        time.sleep(.2);pump()
    assert label in speech()[start:],'Orca reached explicit text-visibility action'
    start=len(speech())
    keyboard.key(0xff0d)
    def changed():
        node.clear_cache();value=Atspi.Text.get_text(node,0,-1)
        return len(value)==3 and (not any(letter in value.lower() for letter in 'qzv') if protected_after else value=='qzv')
    wait(changed,'accessible text changes with explicit visibility action')
    if not protected_after:
        wait(lambda:"SPEECH OUTPUT: 'qzv'" in speech()[start:],'explicitly displayed synthetic value reaches speech')
    time.sleep(.4);pump()
    spoken="SPEECH OUTPUT: 'qzv'" in speech()[start:]
    check(not spoken if protected_after else spoken,'explicit visibility action matches content speech')
    echo_cases.append({'case':'explicitly-masked-again' if protected_after else 'explicitly-revealed-master','role':node.get_role_name(),'typed_character_count':3,'accessible_masked':protected_after,'whole_synthetic_value_spoken':spoken})
    (root/'echo-cases.json').write_text(json.dumps(echo_cases,indent=2,ensure_ascii=False)+'\n')

wait(app,'application')
wait(lambda:speech_log.exists() and 'Starting Atspi main event loop' in speech_log.read_text(errors='replace'),'real Orca ready')
wait(lambda:keyboard.activate_window(pid) or True,'private window activation')
new=wait(lambda:named('データベースを作成','button'),'new database')
focus(new,'new database')
wait(lambda:ui('データベースを作成') in speech(),'Orca names new database action')
check(True,'real Orca receives focus and speaks named create action')
click('データベースを作成')
dialog=wait(lambda:named('データベースを作成','alert'),'credentials')
fs=wait(lambda:fields(dialog) if len(fields(dialog))==3 else None,'credential fields')
for field,value in zip(fs,['Orca 合成検証','synthetic-orca-master-canary','synthetic-orca-master-canary']):
    assert field.get_editable_text_iface().set_text_contents(value)
    focus(field,'credential field')
check([f.get_name() for f in fs]==[ui('データベース名'),ui('マスターパスワード'),ui('マスターパスワード（確認）')],'credential fields have distinct localized accessible names')
wait(lambda:ui('マスターパスワード（確認）') in speech(),'Orca confirmation label')
check(True,'Orca speaks master and confirmation labels')
check(fs[1].get_role_name()=='password text' and fs[2].get_role_name()=='password text','master and confirmation expose protected roles')
check('synthetic-orca-master-canary' not in speech(),'master canary never reaches Orca speech output')
echo_case('normal-database-name',fs[0],False)
assert fs[0].get_editable_text_iface().set_text_contents('Orca 合成検証')
echo_case('masked-master',fs[1],True)
echo_case('masked-confirmation',fs[2],True)
assert fs[2].get_editable_text_iface().set_text_contents('synthetic-orca-master-canary')
toggle_visibility(fs[1],False)
toggle_visibility(fs[1],True)
echo_case('master-masked-again',fs[1],True)
assert fs[1].get_editable_text_iface().set_text_contents('synthetic-orca-master-canary')
click('作成')
wait(lambda:named('Orca 合成検証','label'),'database ready')
click('エントリーを作成')
dialog=wait(lambda:named('エントリーを作成','dialog'),'entry editor')
fs=wait(lambda:fields(dialog) if len(fields(dialog))>=4 else None,'entry fields')
for field,value in zip(fs,['Orca 合成アカウント','synthetic-orca-user','synthetic-orca-entry-canary','https://example.invalid/orca']):
    assert field.get_editable_text_iface().set_text_contents(value)
    focus(field,'entry field')
echo_case('masked-entry-password',fs[2],True)
assert fs[2].get_editable_text_iface().set_text_contents('synthetic-orca-entry-canary')
check(fs[2].get_role_name()=='password text','entry password exposes protected role')
check('synthetic-orca-entry-canary' not in speech(),'entry password canary never reaches Orca speech output')
check(ui('タイトル') in speech() and ui('ユーザー名') in speech(),'Orca speaks entry field labels')
click('エントリーを保存');wait(lambda:not named('エントリーを作成','dialog'),'entry saved')
wait(lambda:named('Orca 合成アカウント'),'entry visible')
row=wait(lambda:named('Orca 合成アカウント','list item'),'entry list row')
selected=row
while selected.get_parent() and not selected.get_parent().get_selection_iface():selected=selected.get_parent()
assert selected.get_parent().get_selection_iface().select_child(selected.get_index_in_parent())
preview=wait(lambda:named('パスワードのプレビュー','password text'),'masked password preview')
check(Atspi.Text.get_text(preview,0,-1)!='synthetic-orca-entry-canary','preview initially masks its password')
focus(preview,'password preview')
check('synthetic-orca-entry-canary' not in speech(),'Orca never speaks the masked preview canary')
focus(wait(lambda:named('エントリー一覧'),'entry list'),'entry list');keyboard.key(0xff0d)
wait(lambda:named('エントリーを編集','dialog'),'Enter opens editor')
check(True,'Enter on the entry list opens the editor')
keyboard.key(0xff1b);wait(lambda:not named('エントリーを編集','dialog'),'Esc closes editor')
search=wait(lambda:named('エントリーを検索'),'search')
focus(search,'search');wait(lambda:ui('エントリーを検索') in speech(),'Orca search speech');check(True,'Orca speaks search label')
assert search.get_editable_text_iface().set_text_contents('missing-orca-synthetic')
wait(lambda:not named('Orca 合成アカウント'),'search excludes entry')
assert search.get_editable_text_iface().set_text_contents('')
wait(lambda:named('Orca 合成アカウント'),'search restored')
focus(wait(lambda:named('ロック','button'),'lock'),'lock');click('ロック')
wait(lambda:named('未保存の変更を破棄しますか？','alert'),'discard dialog');click('破棄')
wait(lambda:named('データベースはロックされています','label'),'locked screen')
check(not named('Orca 合成アカウント'),'locked accessible tree conceals synthetic account')
check('synthetic-orca-master-canary' not in speech() and 'synthetic-orca-entry-canary' not in speech(),'no password canary spoken across creation/edit/search/lock')
print(f'PASS Orca private session: {checks} checks',flush=True)
keyboard.close_window()
keyboard.close()
