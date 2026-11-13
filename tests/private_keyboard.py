"""Keyboard input exclusively for the project's disposable GNOME session.
Never use this API on the physical user's desktop or a non-private bus.
"""
import os
from pathlib import Path
import time
from gi.repository import Gio, GLib

class PrivateKeyboard:
    def __init__(self):
        root=Path(os.environ['KPXC_UI_ROOT']).resolve()
        runtime=Path(os.environ['XDG_RUNTIME_DIR']).resolve()
        assert root in runtime.parents and os.environ.get('GSETTINGS_BACKEND')=='memory'
        assert os.environ.get('WAYLAND_DISPLAY')=='kpxc-gnome-test'
        assert os.environ.get('DBUS_SESSION_BUS_ADDRESS')==os.environ.get('DBUS_SYSTEM_BUS_ADDRESS')
        self.bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
        self.path=self.bus.call_sync('org.gnome.Mutter.RemoteDesktop','/org/gnome/Mutter/RemoteDesktop','org.gnome.Mutter.RemoteDesktop','CreateSession',None,GLib.VariantType('(o)'),Gio.DBusCallFlags.NONE,2000,None).unpack()[0]
        self.call('Start',None)
    def call(self,method,args):
        return self.bus.call_sync('org.gnome.Mutter.RemoteDesktop',self.path,'org.gnome.Mutter.RemoteDesktop.Session',method,args,None,Gio.DBusCallFlags.NONE,2000,None)
    def key(self,keysym):
        self.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(keysym,True)))
        self.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(keysym,False)))
        time.sleep(.08)
    def activate_window(self,pid):
        # Only this private shell enables its test evaluation API. Select the
        # exact synthetic app PID; never request this on the login session.
        code=("Main.overview.hide(); (() => { const a = global.get_window_actors().find(a => a.meta_window.get_pid() === "
              +str(int(pid))+"); if (!a) return false; a.meta_window.activate(global.get_current_time()); return true; })()")
        result=self.bus.call_sync('org.gnome.Shell','/org/gnome/Shell','org.gnome.Shell','Eval',GLib.Variant('(s)',(code,)),GLib.VariantType('(bs)'),Gio.DBusCallFlags.NONE,2000,None).unpack()
        assert result==(True,'true'), 'private test window activation: '+str(result)
        time.sleep(.5)
    def close_window(self):
        self.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe9,True)))
        self.key(0xffc1)  # Alt+F4 is GNOME's keyboard window-close action.
        self.call('NotifyKeyboardKeysym',GLib.Variant('(ub)',(0xffe9,False)))
    def close(self):self.call('Stop',None)
