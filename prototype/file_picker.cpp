#include <gio/gio.h>
#include <glib-unix.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
constexpr auto Portal = "org.freedesktop.portal.Desktop";
constexpr auto DesktopPath = "/org/freedesktop/portal/desktop";

struct Picker
{
    GMainLoop* loop;
    GDBusConnection* bus;
    const std::string& requestPath;
    int result = 1;
};

gboolean cancelRequest(gpointer data)
{
    auto& picker = *static_cast<Picker*>(data);
    auto* reply = g_dbus_connection_call_sync(
        picker.bus, Portal, picker.requestPath.c_str(), "org.freedesktop.portal.Request", "Close",
        nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    if (reply) {
        picker.result = 2;
        g_variant_unref(reply);
    }
    g_main_loop_quit(picker.loop);
    return G_SOURCE_CONTINUE;
}

void responseReceived(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* reply,
                      gpointer data)
{
    auto& picker = *static_cast<Picker*>(data);
    guint code = 2;
    GVariant* results = nullptr;
    g_variant_get(reply, "(u@a{sv})", &code, &results);
    if (code == 0) {
        auto* uris = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
        if (uris && g_variant_n_children(uris) == 1) {
            const gchar* uri = nullptr;
            g_variant_get_child(uris, 0, "&s", &uri);
            gchar* host = nullptr;
            auto* path = g_filename_from_uri(uri, &host, nullptr);
            if (path && (!host || g_str_equal(host, "localhost"))) {
                const auto length = std::strlen(path);
                if (std::fwrite(path, 1, length, stdout) == length && std::fflush(stdout) == 0) {
                    picker.result = 0;
                }
            }
            g_free(path);
            g_free(host);
        }
        g_clear_pointer(&uris, g_variant_unref);
    } else if (code == 1) {
        picker.result = 2;
    }
    g_variant_unref(results);
    g_main_loop_quit(picker.loop);
}
}

int main(int argc, char** argv)
{
    if (argc != 3 || !g_path_is_absolute(argv[1])
        || (*argv[2] && (!g_str_has_prefix(argv[2], "wayland:") || !argv[2][8]))) {
        return 1;
    }
    auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) {
        return 1;
    }

    // Subscribe before calling OpenFile, since a response can arrive immediately.
    auto* uuid = g_uuid_string_random();
    std::string token = "kpxc_phase2_" + std::string(uuid);
    g_free(uuid);
    std::ranges::replace(token, '-', '_');
    std::string sender = g_dbus_connection_get_unique_name(bus) + 1;
    std::ranges::replace(sender, '.', '_');
    const auto requestPath = std::string(DesktopPath) + "/request/" + sender + "/" + token;
    Picker picker{g_main_loop_new(nullptr, FALSE), bus, requestPath};
    const auto cancelSource = g_unix_signal_add(SIGTERM, cancelRequest, &picker);
    const auto subscription = g_dbus_connection_signal_subscribe(
        bus, Portal, "org.freedesktop.portal.Request", "Response", requestPath.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, responseReceived, &picker, nullptr);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    g_variant_builder_add(&options, "{sv}", "current_folder", g_variant_new_bytestring(argv[1]));
    auto* request = g_dbus_connection_call_sync(
        bus, Portal, DesktopPath, "org.freedesktop.portal.FileChooser", "OpenFile",
        g_variant_new("(ssa{sv})", argv[2], "Choose a generated fixture", &options), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, nullptr);
    if (request) {
        const gchar* handle = nullptr;
        g_variant_get(request, "(&o)", &handle);
        if (requestPath == handle) {
            const auto watch = g_bus_watch_name_on_connection(
                bus, Portal, G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
                [](GDBusConnection*, const gchar*, gpointer data) {
                    g_main_loop_quit(static_cast<Picker*>(data)->loop);
                }, &picker, nullptr);
            g_main_loop_run(picker.loop);
            g_bus_unwatch_name(watch);
        }
        g_variant_unref(request);
    }
    g_dbus_connection_signal_unsubscribe(bus, subscription);
    g_source_remove(cancelSource);
    g_main_loop_unref(picker.loop);
    g_object_unref(bus);
    return picker.result;
}
