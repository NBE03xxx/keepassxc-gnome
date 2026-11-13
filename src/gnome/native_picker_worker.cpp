// No Qt, keys, database access or clipboard. Only native picker metadata and a
// selected local path cross this process boundary. exec has replaced the
// parent's address space before this process is allowed to identify to portal.
#include <gio/gio.h>
#include <glib-unix.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>

namespace {
constexpr auto Portal = "org.freedesktop.portal.Desktop";
constexpr auto Desktop = "/org/freedesktop/portal/desktop";
struct Picker {
    GDBusConnection* bus = nullptr;
    GMainLoop* loop = nullptr;
    std::string path, selected;
    unsigned status = 2; // selected=0, cancelled=1, failed=2
    bool stopping = false, completed = false;
    guint watch = 0;
};
void stop(Picker& p) {
    if (p.stopping) return;
    p.stopping = true; p.status = 1;
    g_dbus_connection_call(p.bus, Portal, p.path.c_str(), "org.freedesktop.portal.Request", "Close", nullptr, nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, nullptr, +[](GObject* bus, GAsyncResult* result, gpointer data) {
            auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, nullptr);
            if (reply) g_variant_unref(reply);
            g_main_loop_quit(static_cast<Picker*>(data)->loop);
        }, &p);
}
}
int main(int argc, char** argv) {
    if (argc != 8) return 2;
    const std::string mode(argv[1]);
    if ((mode != "open" && mode != "save" && mode != "folder") || std::strlen(argv[2]) > 512
        || std::strlen(argv[3]) > 4096 || (*argv[3] && !g_path_is_absolute(argv[3])) || std::strlen(argv[4]) > 255
        || std::strchr(argv[4], '/') || std::strlen(argv[5]) > 4096 || (*argv[5] && !g_str_has_prefix(argv[5], "wayland:"))) return 2;
    char* end = nullptr; const auto parent = std::strtoul(argv[6], &end, 10);
    if (!parent || !end || *end || std::strcmp(argv[7], "v1")) return 2;
    const rlimit core{0, 0}, memory{256ULL << 20, 256ULL << 20}, cpu{5, 5};
    if (::setrlimit(RLIMIT_CORE, &core) || ::setrlimit(RLIMIT_AS, &memory) || ::setrlimit(RLIMIT_CPU, &cpu)
        || ::prctl(PR_SET_PDEATHSIG, SIGTERM) || unsigned(::getppid()) != parent) return 2;
    // dumpable remains 1: only paths/UI metadata are handled, allowing portal
    // to inspect /proc/PID/root. No credentials are supplied on argv or stdin.
    Picker p; p.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr); if (!p.bus) return 2;
    p.loop = g_main_loop_new(nullptr, FALSE);
    auto* uuid = g_uuid_string_random(); std::string token = std::string("kpxc_gnome_") + uuid; g_free(uuid);
    std::replace(token.begin(), token.end(), '-', '_');
    std::string sender(g_dbus_connection_get_unique_name(p.bus) + 1); std::replace(sender.begin(), sender.end(), '.', '_');
    p.path = std::string(Desktop) + "/request/" + sender + '/' + token;
    const auto subscription = g_dbus_connection_signal_subscribe(p.bus, Portal, "org.freedesktop.portal.Request", "Response", p.path.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* response, gpointer data) {
            auto& p = *static_cast<Picker*>(data);
            if (p.stopping || p.completed || !g_variant_is_of_type(response, G_VARIANT_TYPE("(ua{sv})"))) return;
            guint code; GVariant* result; g_variant_get(response, "(u@a{sv})", &code, &result);
            if (code == 0) {
                auto* uris = g_variant_lookup_value(result, "uris", G_VARIANT_TYPE_STRING_ARRAY);
                if (uris && g_variant_n_children(uris) == 1) {
                    const char* uri; g_variant_get_child(uris, 0, "&s", &uri);
                    if (std::strlen(uri) <= 8192) {
                        char* host = nullptr; auto* path = g_filename_from_uri(uri, &host, nullptr);
                        if (path && g_path_is_absolute(path) && std::strlen(path) <= 4096 && g_utf8_validate(path, -1, nullptr)
                            && (!host || !*host || g_str_equal(host, "localhost"))) { p.selected = path; p.status = 0; }
                        g_free(host); g_free(path);
                    }
                }
                if (uris) g_variant_unref(uris);
            } else if (code == 1) p.status = 1;
            p.completed = true; g_variant_unref(result); g_main_loop_quit(p.loop);
        }, &p, nullptr);
    GVariantBuilder options; g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    if (*argv[3]) g_variant_builder_add(&options, "{sv}", "current_folder", g_variant_new_bytestring(argv[3]));
    if (mode == "folder") g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(TRUE));
    if (mode == "save" && *argv[4]) g_variant_builder_add(&options, "{sv}", "current_name", g_variant_new_string(argv[4]));
    if (mode != "folder") {
        GVariantBuilder filters; g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
        GVariantBuilder patterns; g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)"));
        g_variant_builder_add(&patterns, "(us)", 0U, "*.[kK][dD][bB][xX]");
        g_variant_builder_add(&filters, "(sa(us))", "KeePass (*.kdbx)", &patterns);
        g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)")); g_variant_builder_add(&patterns, "(us)", 0U, "*");
        g_variant_builder_add(&filters, "(sa(us))", "*", &patterns);
        g_variant_builder_add(&options, "{sv}", "filters", g_variant_builder_end(&filters));
    }
    const auto terminate = g_unix_signal_add(SIGTERM, +[](gpointer data) -> gboolean { stop(*static_cast<Picker*>(data)); return G_SOURCE_CONTINUE; }, &p);
    const auto closed = g_unix_fd_add(STDIN_FILENO, GIOCondition(G_IO_HUP | G_IO_ERR), +[](gint, GIOCondition, gpointer data) -> gboolean { stop(*static_cast<Picker*>(data)); return G_SOURCE_REMOVE; }, &p);
    const auto timeout = g_timeout_add_seconds(600, +[](gpointer data) -> gboolean { stop(*static_cast<Picker*>(data)); return G_SOURCE_REMOVE; }, &p);
    g_dbus_connection_call(p.bus, Portal, Desktop, "org.freedesktop.portal.FileChooser", mode == "save" ? "SaveFile" : "OpenFile",
        g_variant_new("(ssa{sv})", argv[5], argv[2], &options), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 10000, nullptr,
        +[](GObject* bus, GAsyncResult* result, gpointer data) {
            auto& p = *static_cast<Picker*>(data);
            auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, nullptr);
            const char* handle = nullptr; if (reply) g_variant_get(reply, "(&o)", &handle);
            if (!handle || p.path != handle) { p.status = 2; p.completed = true; g_main_loop_quit(p.loop); }
            if (handle && p.path == handle && !p.completed && !p.stopping) {
                p.watch = g_bus_watch_name_on_connection(p.bus, Portal, G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
                    +[](GDBusConnection*, const char*, gpointer data) {
                        auto& p = *static_cast<Picker*>(data); p.status = 2; p.completed = true; g_main_loop_quit(p.loop);
                    }, &p, nullptr);
            }
            if (reply) g_variant_unref(reply);
        }, &p);
    g_main_loop_run(p.loop);
    if (p.watch) g_bus_unwatch_name(p.watch);
    g_source_remove(terminate);
    // Sources may have removed themselves; look up before destroying.
    for (auto id : {closed, timeout}) if (auto* source = g_main_context_find_source_by_id(nullptr, id)) g_source_destroy(source);
    g_dbus_connection_signal_unsubscribe(p.bus, subscription);
    // Fixed-width framing, bounded native UTF-8 path; no toolkit diagnostics.
    const auto length = p.selected.size(); unsigned char header[]{static_cast<unsigned char>(p.status),
        static_cast<unsigned char>(length), static_cast<unsigned char>(length >> 8)};
    const bool sent = std::fwrite(header, 1, sizeof(header), stdout) == sizeof(header)
        && std::fwrite(p.selected.data(), 1, length, stdout) == length && !std::fflush(stdout);
    g_main_loop_unref(p.loop); g_object_unref(p.bus);
    return sent ? 0 : 2;
}
