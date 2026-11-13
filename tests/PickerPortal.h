#pragma once
#include "gnome/NativePicker.h"
#include <sys/prctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <functional>

namespace PickerTest {
inline unsigned checks = 0;
inline void check(bool ok, const char* text) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", text); std::exit(1); } ++checks; }
inline void until(std::function<bool()> predicate, const char* label) {
    const auto end = g_get_monotonic_time() + 8000000;
    while (!predicate() && g_get_monotonic_time() < end) { while (g_main_context_iteration(nullptr, FALSE)) {} g_usleep(1000); }
    check(predicate(), label);
}
struct Portal {
    GDBusConnection* bus;
    GDBusNodeInfo* schema;
    guint object;
    enum Reply { Select, Cancel, Multiple, Remote, Oversized, BadHandle, Hold, Fail } reply = Select;
    unsigned calls = 0, closes = 0;
    bool save = false, folder = false, wayland = false;
    unsigned parentHandles = 0;
    std::string request;
    explicit Portal(const char* address) {
        bus = g_dbus_connection_new_for_address_sync(address, GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
        check(bus, "fake portal private connection");
        const char* xml = "<node><interface name='org.freedesktop.portal.FileChooser'><method name='OpenFile'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method><method name='SaveFile'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method></interface><interface name='org.freedesktop.portal.Request'><method name='Close'/></interface></node>";
        schema = g_dbus_node_info_new_for_xml(xml, nullptr);
        static const GDBusInterfaceVTable chooser{+[](GDBusConnection*, const char* sender, const char*, const char*, const char* method, GVariant* args, GDBusMethodInvocation* invocation, gpointer data) {
            auto& self = *static_cast<Portal*>(data); ++self.calls;
            auto* identity = g_dbus_connection_call_sync(self.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID", g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
            check(identity, "helper process identity"); guint pid; g_variant_get(identity, "(u)", &pid); g_variant_unref(identity);
            const auto proc = "/proc/" + std::to_string(pid);
            auto* root = g_file_read_link((proc + "/root").c_str(), nullptr); check(root, "fresh helper can identify to the desktop portal"); g_free(root);
            char* limits = nullptr; check(g_file_get_contents((proc + "/limits").c_str(), &limits, nullptr, nullptr), "helper limits readable");
            const auto* core = std::strstr(limits, "Max core file size"); unsigned long soft = 1, hard = 1;
            check(core && std::sscanf(core, "Max core file size %lu %lu", &soft, &hard) == 2 && !soft && !hard, "helper core dumps remain disabled"); g_free(limits);
            const char* parent; const char* title; GVariant* options; g_variant_get(args, "(&s&s@a{sv})", &parent, &title, &options);
            check((self.wayland ? g_str_has_prefix(parent, "wayland:") && std::strlen(parent) > 8 : !*parent) && !std::strcmp(title, "合成データを選択"), "localized title and parent metadata");
            if (*parent) ++self.parentHandles;
            check((!std::strcmp(method, "SaveFile")) == self.save, "open/save method selection");
            gboolean modal = FALSE, multiple = TRUE, directory = FALSE;
            check(g_variant_lookup(options, "modal", "b", &modal) && modal && g_variant_lookup(options, "multiple", "b", &multiple) && !multiple, "single modal chooser");
            g_variant_lookup(options, "directory", "b", &directory); check(bool(directory) == self.folder, "directory selection mode");
            if (self.save) { const char* name; check(g_variant_lookup(options, "current_name", "&s", &name) && !std::strcmp(name, "合成.kdbx"), "save filename supplied"); }
            const char* token; check(g_variant_lookup(options, "handle_token", "&s", &token), "predictable request token");
            std::string unique(sender + 1); for (auto& c : unique) if (c == '.') c = '_';
            self.request = "/org/freedesktop/portal/desktop/request/" + unique + '/' + token;
            g_variant_unref(options);
            if (self.reply == Fail) { g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.NotAllowed", "test failure"); return; }
            static const GDBusInterfaceVTable closer{+[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, GDBusMethodInvocation* invocation, gpointer data) {
                ++static_cast<Portal*>(data)->closes; g_dbus_method_invocation_return_value(invocation, nullptr);
            }, nullptr, nullptr, {nullptr}};
            g_dbus_connection_register_object(self.bus, self.request.c_str(), self.schema->interfaces[1], &closer, &self, nullptr, nullptr);
            // Response can arrive immediately, including before method completion.
            if (self.reply != Hold && self.reply != BadHandle) {
                GVariantBuilder values; g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
                const std::string huge = "file:///tmp/" + std::string(9000, 'x');
                const char* paths[]{self.reply == Oversized ? huge.c_str() : self.reply == Remote ? "file://other-host/tmp/remote.kdbx" : "file:///tmp/%E5%90%88%E6%88%90%20test.kdbx", "file:///tmp/second.kdbx", nullptr};
                g_variant_builder_add(&values, "{sv}", "uris", g_variant_new_strv(paths, self.reply == Multiple ? 2 : 1));
                g_dbus_connection_emit_signal(self.bus, sender, self.request.c_str(), "org.freedesktop.portal.Request", "Response", g_variant_new("(ua{sv})", self.reply == Cancel ? 1U : 0U, &values), nullptr);
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", self.reply == BadHandle ? "/unexpected" : self.request.c_str()));
        }, nullptr, nullptr, {nullptr}};
        object = g_dbus_connection_register_object(bus, "/org/freedesktop/portal/desktop", schema->interfaces[0], &chooser, this, nullptr, nullptr);
        auto* result = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        check(result, "fake portal name reply"); guint owned; g_variant_get(result, "(u)", &owned); check(owned == 1, "fake portal owns service name"); g_variant_unref(result);
    }
    ~Portal() { g_dbus_connection_unregister_object(bus, object); g_dbus_connection_close_sync(bus, nullptr, nullptr); g_object_unref(bus); g_dbus_node_info_unref(schema); }
};
}
