#!/usr/bin/env python3
"""Interactive physical-session probe using synthetic values only.
Never reads the clipboard until the user explicitly replaces its old contents.
Records classifications only; never the clipboard text.
"""
import json
from pathlib import Path
import sys
import time
import gi
gi.require_version('Gtk','4.0')
from gi.repository import Gtk, Gdk, GLib
profile=Path(sys.argv[1])
expected='synthetic-host-entry-password'
baseline='synthetic-host-clipboard-baseline'
app=Gtk.Application(application_id='org.keepassxc.Gnome.ClipboardProbe')

def record(phase,text):
    kind='synthetic-password' if text==expected else 'synthetic-baseline' if text==baseline else 'empty' if not text else 'other'
    with (profile/'clipboard-probe.jsonl').open('a') as f:
        f.write(json.dumps({'phase':phase,'classification':kind,'monotonic':time.monotonic()})+'\n')
    return kind

def activate(application):
    window=Gtk.ApplicationWindow(application=application,title='合成データのクリップボード検証')
    window.set_default_size(500,260)
    box=Gtk.Box(orientation=Gtk.Orientation.VERTICAL,spacing=16)
    for edge in ('top','bottom','start','end'):getattr(box,'set_margin_'+edge)(20)
    label=Gtk.Label(label='1. 下のボタンで既存内容を合成文字列へ置換します。\n2. アプリで合成パスワードをコピーします。\n3. すぐ戻り「コピー結果を確認」を押します。\n検証中は他の内容をコピーしないでください。',wrap=True)
    box.append(label);clipboard=window.get_clipboard()
    prepare=Gtk.Button(label='合成文字列で置換して開始');sample=Gtk.Button(label='コピー結果を確認');sample.set_sensitive(False)
    box.append(prepare);box.append(sample);window.set_child(box)
    def start(button):
        clipboard.set(baseline);prepare.set_sensitive(False);sample.set_sensitive(True)
        label.set_label('既存内容を置換しました。アプリで合成パスワードをコピーし、すぐ戻って確認してください。')
    def read(phase):
        def done(cb,res):
            try:
                text=cb.read_text_finish(res);kind=record(phase,text);del text
                label.set_label(('コピー直後' if phase=='immediate' else '12秒待機後')+'：'+kind)
                if phase=='immediate':GLib.timeout_add_seconds(12,lambda:(read('after-expiry'),False)[1])
                else:
                    clipboard.set(baseline)
                    label.set_label('検証終了：'+kind+'。クリップボードは合成の基準文字列へ戻しました。')
            except Exception as error:
                with (profile/'clipboard-probe.jsonl').open('a') as f:f.write(json.dumps({'phase':phase,'error':type(error).__name__})+'\n')
        clipboard.read_text_async(None,done)
    def sample_now(button):sample.set_sensitive(False);read('immediate')
    prepare.connect('clicked',start);sample.connect('clicked',sample_now)
    window.present()
app.connect('activate',activate);app.run([])
