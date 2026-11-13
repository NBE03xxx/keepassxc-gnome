#!/usr/bin/env python3
"""Explicit commands against only the PID of our synthetic physical-session test.
Never walk another application or read a pre-existing clipboard.
"""
import json
import hashlib
import subprocess
from pathlib import Path
import sys
import time
import gi
gi.require_version('Atspi','2.0')
from gi.repository import Atspi, Gio, GLib

ROOT = Path(__file__).resolve().parents[1]
PROFILE = Path((ROOT/'.cache/host-final-root.txt').read_text().strip())
PID = int((PROFILE/'pid').read_text())
MASTER = 'synthetic-host-final-master'
DB = '実機検証用（合成データ）'
ENTRY = '合成アカウント / Host final'

def walk(item, depth=0):
    if item is None or depth > 40: return
    item.clear_cache()
    yield item
    for i in range(item.get_child_count()):
        child = item.get_child_at_index(i)
        if child is not None: yield from walk(child, depth+1)

def app():
    for child in Atspi.get_desktop(0):
        if child is not None and child.get_process_id() == PID:
            child.set_cache_mask(Atspi.Cache.NONE);child.clear_cache();return child

def named(text, role=None, root=None):
    for item in walk(root if root is not None else app()):
        if item.get_state_set().contains(Atspi.StateType.SHOWING) and item.get_name().strip() == text and (role is None or role in item.get_role_name()): return item

def wait(predicate, label, seconds=30):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        for _ in range(20):
            if not GLib.MainContext.default().iteration(False): break
        try:
            result=predicate()
            if result:return result
        except Exception:pass
        time.sleep(.1)
    raise AssertionError(label)

def click(text, root=None):
    node=wait(lambda:named(text,'button',root), 'button '+text)
    wait(lambda:node.get_state_set().contains(Atspi.StateType.SENSITIVE),'sensitive '+text)
    assert node.get_action_iface().do_action(0),text

def fields(root):
    return [item for item in walk(root) if item.get_role_name() in ('text','password text') and item.get_editable_text_iface()]

def set_text(item,value): assert item.get_editable_text_iface().set_text_contents(value)

def select_entry():
    row=wait(lambda:named(ENTRY,'list item'),'synthetic entry')
    while row.get_parent() and not row.get_parent().get_selection_iface():row=row.get_parent()
    assert row.get_parent().get_selection_iface().select_child(row.get_index_in_parent())
    wait(lambda:row.get_state_set().contains(Atspi.StateType.SELECTED),'synthetic selection')

def status():
    labels=[]
    for item in walk(app()):
        if item.get_role_name() in ('label','alert','dialog','frame'):labels.append([item.get_role_name(),item.get_name()])
    return labels

def record(label):
    with (PROFILE/'checks.jsonl').open('a') as file:file.write(json.dumps({'check':label,'status':'pass'},ensure_ascii=False)+'\n')
    print('PASS',label,flush=True)

if __name__=='__main__':
    command=sys.argv[1]
    wait(app,'test application PID')
    if command=='bootstrap':
        if (PROFILE/'fixtures/host-native.kdbx').exists():
            raise SystemExit('Saved fixture already exists: reopen it; never create a second unsaved test database.')
        click('データベースを作成'); dialog=wait(lambda:named('データベースを作成','alert'),'creation dialog')
        fs=wait(lambda:fields(dialog) if len(fields(dialog))==3 else None,'creation fields')
        for field,value in zip(fs,[DB,MASTER,MASTER]):set_text(field,value)
        click('作成',dialog);wait(lambda:named(DB,'label'),'new synthetic database')
        click('グループを作成');dialog=wait(lambda:named('グループを作成','alert'),'new group')
        set_text(wait(lambda:fields(dialog)[0] if fields(dialog) else None,'group field'),'合成グループ');click('保存',dialog);wait(lambda:named('合成グループ','label'),'synthetic group')
        click('エントリーを作成');dialog=wait(lambda:named('エントリーを作成','dialog'),'entry form')
        fs=wait(lambda:fields(dialog) if len(fields(dialog))>=4 else None,'populated entry fields')
        for field,value in zip(fs,[ENTRY,'synthetic-host-user','synthetic-host-entry-password','https://example.invalid/host-final']):set_text(field,value)
        click('エントリーを保存',dialog);wait(lambda:not named('エントリーを作成','dialog'),'entry form closed')
        wait(lambda:named(ENTRY),'synthetic entry created')
        search=wait(lambda:named('エントリーを検索'),'search field')
        set_text(search,'missing-synthetic-host-query');wait(lambda:not named(ENTRY),'nonmatching search')
        set_text(search,'');wait(lambda:named(ENTRY),'search cleared');select_entry()
        click('編集');dialog=wait(lambda:named('エントリーを編集','dialog'),'entry editor')
        fs=wait(lambda:fields(dialog) if len(fields(dialog))>=4 else None,'populated edit fields');assert Atspi.Text.get_text(fs[1],0,-1)=='synthetic-host-user'
        set_text(fs[3],'https://example.invalid/verified-host-final')
        click('エントリーを保存',dialog);wait(lambda:not named('エントリーを編集','dialog'),'edit closed')
        record('physical-session Japanese GUI: synthetic creation/group/entry/search/edit at native desktop settings')
    elif command=='finish-edit':
        dialog=wait(lambda:named('エントリーを編集','dialog'),'entry editor')
        fs=wait(lambda:fields(dialog) if len(fields(dialog))>=4 else None,'populated edit fields')
        assert Atspi.Text.get_text(fs[1],0,-1)=='synthetic-host-user'
        set_text(fs[3],'https://example.invalid/verified-host-final');click('エントリーを保存',dialog)
        wait(lambda:not named('エントリーを編集','dialog'),'edit closed')
        record('candidate Japanese GUI: synthetic creation/group/entry/search/edit')
    elif command=='save':click('保存');record('native file chooser requested from physical-session test application')
    elif command=='prepare-native':
        bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
        pid=bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetConnectionUnixProcessID',GLib.Variant('(s)',('org.gnome.Nautilus',)),GLib.VariantType('(u)'),Gio.DBusCallFlags.NONE,1000,None).unpack()[0]
        def filename():
            for application in Atspi.get_desktop(0):
                if application is None or application.get_process_id()!=pid:continue
                application.set_cache_mask(Atspi.Cache.NONE);application.clear_cache()
                for frame in application:
                    frame.clear_cache()
                    if frame.get_name()!='新しいファイルに保存':continue
                    for field in fields(frame):
                        if field.get_name()=='ファイル名':return field
        set_text(wait(filename,'live native filename'),'host-native.kdbx')
        record('native save chooser ready with synthetic basename; user selects explicit directory')
    elif command=='native-save':
        bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
        pid=bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetConnectionUnixProcessID',GLib.Variant('(s)',('org.gnome.Nautilus',)),GLib.VariantType('(u)'),Gio.DBusCallFlags.NONE,1000,None).unpack()[0]
        def chooser():
            for application in Atspi.get_desktop(0):
                if application is not None and application.get_process_id()==pid:
                    application.set_cache_mask(Atspi.Cache.NONE);application.clear_cache()
                    for frame in application:
                        if frame.get_name()=='新しいファイルに保存' and frame.get_state_set().contains(Atspi.StateType.SHOWING):return frame
        frame=wait(chooser,'our native save chooser')
        filename=wait(lambda:named('ファイル名','text',frame),'native filename')
        target=PROFILE/'fixtures/host-native.kdbx'
        set_text(filename,str(target));click('保存(S)',frame)
        wait(target.is_file,'native save creates expected synthetic path')
        record('real GNOME/Nautilus file chooser saved synthetic KDBX in explicit fixture directory')
    elif command=='open':click('データベースを開く')
    elif command=='unlock':
        dialog=wait(lambda:named('データベースのロック解除','alert'),'unlock dialog')
        fs=wait(lambda:fields(dialog) if len(fields(dialog))==1 else None,'unlock field');set_text(fs[0],MASTER);click('ロック解除',dialog)
        wait(lambda:named(DB,'label'),'synthetic database unlocked');record('native chooser selection and saved synthetic database reopened')
    elif command=='verify-lock':
        wait(lambda:named('データベースはロックされています','label'),'locked database screen')
        assert not named(ENTRY), 'entry remains visible'
        assert not named('エントリーを編集','dialog'), 'editor remains visible'
        assert not [f for f in fields(app()) if f.get_role_name()=='password text'], 'password field remains'
        children=subprocess.run(['ps','--ppid',str(PID),'-o','pid=,comm='],capture_output=True,text=True)
        assert children.returncode in (0,1) and not children.stdout.strip(), 'worker remains after lock'
        baseline_path=max(PROFILE.glob('installed-dev*.json'),key=lambda p:p.stat().st_mtime_ns)
        baseline=json.loads(baseline_path.read_text())
        assert hashlib.sha256((PROFILE/'fixtures/host-native.kdbx').read_bytes()).hexdigest()==baseline['fixture_sha256'], 'saved fixture changed'
        record('installed '+baseline['version']+' locked: no entry/editor/password field/worker; saved fixture hash unchanged')
    elif command=='status':print(json.dumps(status(),ensure_ascii=False,indent=2))
    elif command in ('edit','draft'):
        select_entry();click('編集');dialog=wait(lambda:named('エントリーを編集','dialog'),'editor')
        if command=='draft':
            fs=wait(lambda:fields(dialog) if len(fields(dialog))>=4 else None,'draft fields')
            set_text(fs[0],'Unconfirmed synthetic lock/sleep draft')
            record('unconfirmed synthetic form prepared for physical lock/sleep')
    elif command=='lock':
        click('ロック')
        if named('未保存の変更を破棄しますか？','alert'):click('破棄')
        wait(lambda:not named(ENTRY),'entry concealed');record('manual lock conceals synthetic entries')
    elif command=='close':
        frame=wait(lambda:named('KeePassXC GNOME — 開発版','frame'),'test frame')
        action=frame.get_action_iface()
        if action:
            names=[action.get_action_name(i) for i in range(action.get_n_actions())]
            print('Frame actions:',names)
        click('閉じる')
    else:raise SystemExit('Unknown explicit test command')
