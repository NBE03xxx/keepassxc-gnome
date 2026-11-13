#!/usr/bin/env python3
"""Drive only the synthetic app on a private AT-SPI bus with real Orca running."""
import os
import re
from pathlib import Path
import time
import gi
gi.require_version('Atspi','2.0')
from gi.repository import Atspi,GLib
root=Path(os.environ['KPXC_UI_ROOT']);pid=int(os.environ['KPXC_UI_PID'])
english=os.environ.get('KPXC_UI_LANGUAGE')=='en'
translations={'データベースを作成':'New database','作成':'Create','エントリーを作成':'New entry','エントリーを保存':'Save entry','エントリーを検索':'Search entries','ロック':'Lock','未保存の変更を破棄しますか？':'Discard unsaved changes?','破棄':'Discard','データベースはロックされています':'Your database is locked','閉じる':'Close','データベース名':'Database name','マスターパスワード':'Master password','マスターパスワード（確認）':'Repeat master password','タイトル':'Title','ユーザー名':'Username'}
def ui(text):return translations.get(text,text) if english else text
speech_log=root/'orca-debug.log'
from private_keyboard import PrivateKeyboard
keyboard=PrivateKeyboard()
checks=0

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
        if node.get_state_set().contains(Atspi.StateType.FOCUSED):
            time.sleep(.3);pump();return
        keyboard.key(0xff09);pump()
        # Focus is checked against the live accessible on the next iteration.
    raise AssertionError(label+' reachable with Tab')

def check(ok,label):
    global checks
    assert ok,label
    checks+=1;print('PASS',label,flush=True)

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
focus(fs[1],'master keyboard echo')
start=len(speech())
for letter in 'qzv':keyboard.key(ord(letter))
time.sleep(.4);pump()
check(not re.search(r"SPEECH OUTPUT: '[qzv]'",speech()[start:],re.I),'typed master characters are not echoed as letters')
assert fs[1].get_editable_text_iface().set_text_contents('synthetic-orca-master-canary')
click('作成')
wait(lambda:named('Orca 合成検証','label'),'database ready')
click('エントリーを作成')
dialog=wait(lambda:named('エントリーを作成','dialog'),'entry editor')
fs=wait(lambda:fields(dialog) if len(fields(dialog))>=4 else None,'entry fields')
for field,value in zip(fs,['Orca 合成アカウント','synthetic-orca-user','synthetic-orca-entry-canary','https://example.invalid/orca']):
    assert field.get_editable_text_iface().set_text_contents(value)
    focus(field,'entry field')
focus(fs[2],'entry keyboard echo')
start=len(speech())
for letter in 'qzv':keyboard.key(ord(letter))
time.sleep(.4);pump()
check(not re.search(r"SPEECH OUTPUT: '[qzv]'",speech()[start:],re.I),'typed entry password characters are not echoed as letters')
assert fs[2].get_editable_text_iface().set_text_contents('synthetic-orca-entry-canary')
check(fs[2].get_role_name()=='password text','entry password exposes protected role')
check('synthetic-orca-entry-canary' not in speech(),'entry password canary never reaches Orca speech output')
check(ui('タイトル') in speech() and ui('ユーザー名') in speech(),'Orca speaks entry field labels')
click('エントリーを保存');wait(lambda:not named('エントリーを作成','dialog'),'entry saved')
wait(lambda:named('Orca 合成アカウント'),'entry visible')
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
