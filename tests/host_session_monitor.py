#!/usr/bin/env python3
"""Observe real desktop signals and only our test PID/children/inhibitor.
Does not lock, suspend, alter settings, or read clipboard/database contents.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime
from zoneinfo import ZoneInfo
import gi
from gi.repository import Gio, GLib
profile=Path(sys.argv[1]);pid=int(sys.argv[2]);log=(profile/'session-events.jsonl').open('a',buffering=1)
started=time.monotonic();last=None;last_inhibitors=None;queries=False
session=Gio.bus_get_sync(Gio.BusType.SESSION,None);system=Gio.bus_get_sync(Gio.BusType.SYSTEM,None)

def record(event,**values):
 log.write(json.dumps({'time':datetime.now(ZoneInfo('Asia/Tokyo')).isoformat(),'boot_us':time.clock_gettime_ns(time.CLOCK_BOOTTIME)//1000,'monotonic_us':time.monotonic_ns()//1000,'event':event,**values},ensure_ascii=False)+'\n')

def signal_event(label):
 def signal(connection,sender,path,interface,signal,args,data):
  record(label,value=args.unpack()[0])
 return signal
session.signal_subscribe('org.gnome.ScreenSaver','org.gnome.ScreenSaver','ActiveChanged','/org/gnome/ScreenSaver',None,Gio.DBusSignalFlags.NONE,signal_event('screen-lock'),None)
system.signal_subscribe('org.freedesktop.login1','org.freedesktop.login1.Manager','PrepareForSleep','/org/freedesktop/login1',None,Gio.DBusSignalFlags.NONE,signal_event('sleep'),None)

def poll():
 global last,queries
 try:os.kill(pid,0)
 except ProcessLookupError:record('application-exited');loop.quit();return False
 result=subprocess.run(['ps','--ppid',str(pid),'-o','pid=,comm='],text=True,capture_output=True,check=False)
 if result.returncode not in (0,1):
  record('process-query-error',code=result.returncode)
 children=result.stdout.strip().splitlines()
 state=tuple(line.strip() for line in children)
 if state!=last:record('children',processes=list(state));last=state
 if not queries:
  queries=True
  def result(connection,res,data):
   global queries,last_inhibitors
   queries=False
   try:
    rows=connection.call_finish(res).unpack()[0]
    own=[{'what':r[0],'who':r[1],'mode':r[3]} for r in rows if r[5]==pid]
    if own!=last_inhibitors:
     record('inhibitor',owned=own);last_inhibitors=own
   except Exception as error:record('inhibitor-query-error',kind=type(error).__name__)
  system.call('org.freedesktop.login1','/org/freedesktop/login1','org.freedesktop.login1.Manager','ListInhibitors',None,GLib.VariantType('(a(ssssuu))'),Gio.DBusCallFlags.NO_AUTO_START,1000,None,result,None)
 return True

record('monitor-start',pid=pid)
loop=GLib.MainLoop();GLib.timeout_add(250,poll);loop.run();log.close()
