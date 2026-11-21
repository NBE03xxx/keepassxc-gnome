// SPDX-License-Identifier: GPL-3.0-or-later
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {getInputSourceManager} from 'resource:///org/gnome/shell/ui/status/keyboard.js';
import {getIBusManager} from 'resource:///org/gnome/shell/misc/ibusManager.js';

const PATH = '/org/keepassxc/Gnome/AutoTypeIme';
const INTERFACE = 'org.keepassxc.Gnome.AutoTypeIme';
const HELPER = '/usr/libexec/keepassxc-gnome/keepassxc-gnome-auto-type';
const API = `<node><interface name="${INTERFACE}">
    <method name="Register"><arg type="b" direction="out"/></method>
    <method name="Begin"><arg type="b" direction="out"/></method>
    <method name="End"><arg type="b" direction="out"/></method>
    <signal name="Revoked"/>
</interface></node>`;

const sameSource = (a, b) => a?.type === b?.type && a?.id === b?.id;

export default class AutoTypeImeExtension extends Extension {
    enable() {
        this._bus = Gio.DBus.session;
        this._manager = getInputSourceManager();
        this._ibus = getIBusManager()._ibus;
        this._clients = new Map();
        this._lease = null;
        this._focus = global.display.connect('notify::focus-window', () => this._checkLease());
        this._source = this._manager.connect('current-source-changed', () => this._checkLease());
        this._session = Main.sessionMode.connect('updated', () => this._checkLease());
        this._engine = this._ibus.connect('global-engine-changed', (_bus, name) => {
            const lease = this._lease;
            if (lease?.enginePrefix && lease.ready && !name.startsWith(lease.enginePrefix))
                this._finish(lease, true);
        });
        this._service = Gio.DBusExportedObject.wrapJSObject(API, this);
        this._service.export(this._bus, PATH);
    }

    disable() {
        if (this._lease)
            this._finish(this._lease, true);
        for (const watch of this._clients.values())
            Gio.bus_unwatch_name(watch);
        this._clients.clear();
        global.display.disconnect(this._focus);
        this._manager.disconnect(this._source);
        Main.sessionMode.disconnect(this._session);
        this._ibus.disconnect(this._engine);
        this._service.unexport();
        this._service = null;
    }

    async RegisterAsync(_params, invocation) {
        const sender = invocation.get_sender();
        const bus = this._bus;
        const clients = this._clients;
        let accepted = false;
        try {
            const reply = await new Promise((resolve, reject) => {
                bus.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                    'GetConnectionUnixProcessID', new GLib.Variant('(s)', [sender]), new GLib.VariantType('(u)'),
                    Gio.DBusCallFlags.NO_AUTO_START, 1000, null, (connection, result) => {
                        try {
                            resolve(connection.call_finish(result));
                        } catch (error) {
                            reject(error);
                        }
                    });
            });
            const [pid] = reply.deepUnpack();
            accepted = this._service && this._clients === clients && GLib.file_read_link(`/proc/${pid}/exe`) === HELPER;
            if (accepted && !clients.has(sender)) {
                clients.set(sender, Gio.bus_watch_name_on_connection(bus, sender, Gio.BusNameWatcherFlags.NONE,
                    null, () => {
                        const watch = clients.get(sender);
                        if (watch)
                            Gio.bus_unwatch_name(watch);
                        clients.delete(sender);
                        if (this._lease?.sender === sender)
                            this._finish(this._lease, false);
                    }));
            }
        } catch (_error) {
            // A vanished client or a process outside the installed helper is ineligible.
        }
        invocation.return_value(new GLib.Variant('(b)', [Boolean(accepted)]));
    }

    async BeginAsync(_params, invocation) {
        const sender = invocation.get_sender();
        let ready = false;
        if (this._clients.has(sender) && !this._lease) {
            try {
                ready = await this._begin(sender);
            } catch (error) {
                if (this._lease?.sender === sender)
                    this._finish(this._lease, false);
                console.warn(`KeePassXC GNOME Auto-Type IME: ${error.message}`);
            }
        }
        invocation.return_value(new GLib.Variant('(b)', [ready]));
    }

    EndAsync(_params, invocation) {
        const sender = invocation.get_sender();
        const lease = this._lease;
        const restored = this._clients.has(sender) && (!lease || lease.sender === sender && this._finish(lease, false));
        invocation.return_value(new GLib.Variant('(b)', [restored]));
    }

    async _begin(sender) {
        const original = this._manager.currentSource;
        const window = global.display.focus_window;
        if (!this._canUseWindow(window) || !original)
            return false;
        const switching = original.type === 'ibus';
        const direct = switching ? this._directSource(original) : original;
        if (!direct || direct.type !== 'xkb' || (switching && (this._manager._sourcesPerWindow ||
            window.get_client_type() !== Meta.WindowClientType.WAYLAND)))
            return false;

        const [layout, variant = ''] = direct.id.split('+');
        const lease = {sender, window, original, direct, switched: false, ready: false, timer: 0,
            enginePrefix: this._ibus.is_connected() ? `xkb:${layout}:${variant}:` : null};
        this._lease = lease;
        lease.timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 30, () => {
            lease.timer = 0;
            this._finish(lease, true);
            return GLib.SOURCE_REMOVE;
        });
        if (switching) {
            if (!lease.enginePrefix || !await this._canSwitchIme() || !this._valid(lease)) {
                this._finish(lease, false);
                return false;
            }
            lease.switched = true;
            direct.activate(false);
        }
        lease.ready = !lease.enginePrefix || await this._waitForDirectEngine(lease);
        if (!lease.ready)
            this._finish(lease, false);
        return lease.ready;
    }

    _directSource(original) {
        const layouts = Object.values(this._manager.inputSources).filter(source => source.type === 'xkb');
        return layouts.find(source => source.xkbId === original.xkbId) ??
            (layouts.length === 1 ? layouts[0] : null);
    }

    _canUseWindow(window) {
        return window && window === global.display.focus_window && !Main.sessionMode.isLocked && !Main.overview.visible &&
            window.get_gtk_application_id() !== 'org.keepassxc.Gnome.Development';
    }

    async _canSwitchIme() {
        const method = Main.inputMethod;
        if (!method?._context || method._preeditStr)
            return false;
        const bus = this._ibus;
        const focus = await new Promise(resolve => {
            bus.current_input_context_async(500, null, (connection, result) => {
                try {
                    resolve(connection.current_input_context_async_finish(result));
                } catch (_error) {
                    resolve(null);
                }
            });
        });
        return Boolean(focus && focus === method._context?.get_object_path() && !method._preeditStr);
    }

    async _waitForDirectEngine(lease) {
        const deadline = GLib.get_monotonic_time() + 2_000_000;
        while (this._valid(lease) && GLib.get_monotonic_time() < deadline) {
            const engine = await this._ibus.get_global_engine_async(450, null);
            if (engine?.get_name().startsWith(lease.enginePrefix))
                return this._valid(lease);
            await new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, 45, () => {
                resolve();
                return GLib.SOURCE_REMOVE;
            }));
        }
        return false;
    }

    _valid(lease) {
        return this._lease === lease && this._canUseWindow(lease.window) &&
            sameSource(this._manager.currentSource, lease.switched ? lease.direct : lease.original);
    }

    _checkLease() {
        const lease = this._lease;
        if (lease && !this._valid(lease))
            this._finish(lease, true);
    }

    _finish(lease, notify) {
        if (this._lease !== lease)
            return true;
        this._lease = null;
        if (lease.timer)
            GLib.source_remove(lease.timer);
        if (notify)
            this._bus.emit_signal(lease.sender, PATH, INTERFACE, 'Revoked', null);
        if (!lease.switched || !sameSource(this._manager.currentSource, lease.direct))
            return true;

        const original = Object.values(this._manager.inputSources).find(source => sameSource(source, lease.original));
        if (original) {
            original.activate(false);
            return true;
        }
        // GNOME 50 hides IBus sources while a password field is focused. Put
        // the original source first in the list GNOME restores on leaving it.
        const backup = this._manager._mruSourcesBackup;
        if (this._manager._disableIBus && Array.isArray(backup) && sameSource(backup[0], lease.direct)) {
            const index = backup.findIndex(source => sameSource(source, lease.original));
            if (index > 0) {
                backup.unshift(...backup.splice(index, 1));
                return true;
            }
        }
        return false;
    }
}
