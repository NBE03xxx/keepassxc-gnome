#include "gnome/AutoTypePortal.h"

#include <algorithm>
#include <sys/prctl.h>

namespace
{
constexpr auto Destination = "org.freedesktop.portal.Desktop";
constexpr auto Desktop = "/org/freedesktop/portal/desktop";
constexpr auto Remote = "org.freedesktop.portal.RemoteDesktop";
constexpr auto Shortcuts = "org.freedesktop.portal.GlobalShortcuts";
constexpr auto Session = "org.freedesktop.portal.Session";
constexpr auto Request = "org.freedesktop.portal.Request";
constexpr auto ShortcutId = "auto-type-selected-entry";
constexpr auto Shell = "org.gnome.Shell";
constexpr auto Ime = "org.keepassxc.Gnome.AutoTypeIme";
constexpr auto ImePath = "/org/keepassxc/Gnome/AutoTypeIme";
constexpr auto InstalledHelper = "/usr/libexec/keepassxc-gnome/keepassxc-gnome-auto-type";
constexpr unsigned Keyboard = 1;

std::string uniqueOwner(GDBusConnection* bus, const char* name)
{
    AutoTypePortal::Variant reply{g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", name),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr), g_variant_unref};
    if (!reply) return {};
    const char* owner;
    g_variant_get(reply.get(), "(&s)", &owner);
    return owner;
}

std::string token()
{
    auto* uuid = g_uuid_string_random();
    auto result = std::string("kpxc_") + uuid;
    g_free(uuid);
    std::replace(result.begin(), result.end(), '-', '_');
    return result;
}
}

AutoTypePortal::~AutoTypePortal()
{
    if (bus) {
        endIme();
        for (const auto& session : {remote, shortcut}) {
            if (!session.empty() && !owner.empty()) call(session, Session, "Close", nullptr, nullptr);
        }
        g_dbus_connection_close_sync(bus, nullptr, nullptr);
        g_object_unref(bus);
    }
}

bool AutoTypePortal::connect()
{
    auto* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!address) return false;
    bus = g_dbus_connection_new_for_address_sync(address,
        GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, nullptr);
    g_free(address);
    if (!bus || !g_dbus_connection_get_unique_name(bus)) return false;
    g_dbus_connection_set_exit_on_close(bus, FALSE);
    sender = g_dbus_connection_get_unique_name(bus) + 1;
    std::replace(sender.begin(), sender.end(), '.', '_');
    // Portal identity must be registered before it accepts other calls. No
    // credential data is read while the portal can inspect /proc/PID/root.
    owner = Destination;
    if (!call(Desktop, "org.freedesktop.host.portal.Registry", "Register",
              g_variant_new("(sa{sv})", "org.keepassxc.Gnome.Development", nullptr), G_VARIANT_TYPE("()"))) return false;
    // The Shell extension also checks /proc/PID/exe. Register on the same
    // private connection before making this process unreadable.
    imeRegistrationFailed = !connectIme();
    if (::prctl(PR_SET_DUMPABLE, 0) || imeRegistrationFailed) return false;

    owner = uniqueOwner(bus, Destination);
    if (owner.empty()) return false;
    g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", Destination, G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
            static_cast<AutoTypePortal*>(data)->cancel();
        }, this, nullptr);
    return true;
}

bool AutoTypePortal::connectIme()
{
    const std::unique_ptr<char, decltype(&g_free)> image{g_file_read_link("/proc/self/exe", nullptr), g_free};
    if (g_strcmp0(image.get(), InstalledHelper) != 0) return true;
    imeOwner = uniqueOwner(bus, Shell);
    if (imeOwner.empty()) return true;
    GError* error = nullptr;
    if (!callIme("Register", &error)) {
        const bool absent = g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)
            || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT)
            || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE);
        g_clear_error(&error);
        imeOwner.clear();
        return absent;
    }
    g_dbus_connection_signal_subscribe(bus, imeOwner.c_str(), Ime, "Revoked", ImePath,
        nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
            static_cast<AutoTypePortal*>(data)->cancel();
        }, this, nullptr);
    g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", Shell, G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
            static_cast<AutoTypePortal*>(data)->cancel();
        }, this, nullptr);
    return true;
}

bool AutoTypePortal::callIme(const char* method, GError** error) const
{
    Variant result{g_dbus_connection_call_sync(bus, imeOwner.c_str(), ImePath, Ime, method,
        nullptr, G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, error), g_variant_unref};
    gboolean allowed = FALSE;
    if (result) g_variant_get(result.get(), "(b)", &allowed);
    return allowed;
}

bool AutoTypePortal::beginIme()
{
    if (!imeAvailable()) return true;
    // A timed-out Begin may have taken effect. End or disconnect will restore it.
    imeActive = true;
    return callIme("Begin");
}

bool AutoTypePortal::endIme()
{
    if (!imeActive) return true;
    imeActive = false;
    return callIme("End");
}

AutoTypePortal::Variant AutoTypePortal::call(const std::string& path, const char* interface, const char* method,
                                           GVariant* args, const GVariantType* type) const
{
    return {g_dbus_connection_call_sync(bus, owner.c_str(), path.c_str(), interface, method, args, type,
        owner == Destination ? G_DBUS_CALL_FLAGS_NONE : G_DBUS_CALL_FLAGS_NO_AUTO_START,
        2000, nullptr, nullptr), g_variant_unref};
}

bool AutoTypePortal::waitFor(unsigned milliseconds, const std::function<bool()>& until) const
{
    bool expired = false;
    auto* timeout = g_timeout_source_new(milliseconds);
    g_source_set_callback(timeout, +[](gpointer data) -> gboolean {
        *static_cast<bool*>(data) = true;
        return G_SOURCE_REMOVE;
    }, &expired, nullptr);
    g_source_attach(timeout, nullptr);
    while (!stopped && !expired && !until()) g_main_context_iteration(nullptr, TRUE);
    g_source_destroy(timeout);
    g_source_unref(timeout);
    return !stopped && !expired && until();
}

AutoTypePortal::Variant AutoTypePortal::request(const char* interface, const char* method, unsigned milliseconds,
                                               const std::function<GVariant*(const char*)>& args) const
{
    const auto requestToken = token();
    const auto path = std::string(Desktop) + "/request/" + sender + "/" + requestToken;
    struct Reply {
        bool received = false;
        Variant results{nullptr, g_variant_unref};
    } reply;
    const auto subscription = g_dbus_connection_signal_subscribe(bus, owner.c_str(), Request, "Response",
        path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters, gpointer data) {
            auto& reply = *static_cast<Reply*>(data);
            if (reply.received || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) return;
            guint status;
            GVariant* results;
            g_variant_get(parameters, "(u@a{sv})", &status, &results);
            reply.received = true;
            if (status == 0) reply.results.reset(results);
            else g_variant_unref(results);
        }, &reply, nullptr);
    const auto opened = call(Desktop, interface, method, args(requestToken.c_str()), G_VARIANT_TYPE("(o)"));
    const char* returned = nullptr;
    if (opened) g_variant_get(opened.get(), "(&o)", &returned);
    const bool complete = returned && path == returned && waitFor(milliseconds, [&] { return reply.received; });
    g_dbus_connection_signal_unsubscribe(bus, subscription);
    if (!complete) {
        call(path, Request, "Close", nullptr, nullptr);
        return {nullptr, g_variant_unref};
    }
    return std::move(reply.results);
}

bool AutoTypePortal::createSession(const char* interface, std::string& session)
{
    const auto sessionToken = token();
    session = std::string(Desktop) + "/session/" + sender + "/" + sessionToken;
    const auto result = request(interface, "CreateSession", 2500, [&](const char* requestToken) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(requestToken));
        g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(sessionToken.c_str()));
        return g_variant_new("(a{sv})", &options);
    });
    const char* path = nullptr;
    if (!result || (!g_variant_lookup(result.get(), "session_handle", "&o", &path)
                 && !g_variant_lookup(result.get(), "session_handle", "&s", &path))
        || session != path) return false;
    g_dbus_connection_signal_subscribe(bus, owner.c_str(), Session, "Closed", session.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
            static_cast<AutoTypePortal*>(data)->cancel();
        }, this, nullptr);
    return true;
}

bool AutoTypePortal::prepare(const char* parent, const char* shortcutDescription)
{
    if (!createSession(Shortcuts, shortcut)) return false;
    if (!request(Shortcuts, "BindShortcuts", 120000, [&](const char* requestToken) {
        GVariantBuilder binding, shortcuts, options;
        g_variant_builder_init(&binding, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&binding, "{sv}", "description", g_variant_new_string(shortcutDescription));
        g_variant_builder_add(&binding, "{sv}", "preferred_trigger", g_variant_new_string("CTRL+ALT+u"));
        g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
        g_variant_builder_add(&shortcuts, "(sa{sv})", ShortcutId, &binding);
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(requestToken));
        return g_variant_new("(oa(sa{sv})sa{sv})", shortcut.c_str(), &shortcuts, parent, &options);
    })) return false;
    if (!createSession(Remote, remote)) return false;
    if (!request(Remote, "SelectDevices", 2500, [&](const char* requestToken) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(requestToken));
        g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(Keyboard));
        g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(0));
        return g_variant_new("(oa{sv})", remote.c_str(), &options);
    })) return false;
    const auto result = request(Remote, "Start", 120000, [&](const char* requestToken) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(requestToken));
        return g_variant_new("(osa{sv})", remote.c_str(), parent, &options);
    });
    guint devices = 0;
    gboolean clipboard = FALSE;
    if (!result || !g_variant_lookup(result.get(), "devices", "u", &devices) || devices != Keyboard) return false;
    g_variant_lookup(result.get(), "clipboard_enabled", "b", &clipboard);
    if (clipboard) return false;
    g_dbus_connection_signal_subscribe(bus, owner.c_str(), Shortcuts, nullptr, Desktop, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const char*, const char*, const char*, const char* signal, GVariant* parameters, gpointer data) {
            auto& portal = *static_cast<AutoTypePortal*>(data);
            if (!portal.armed || portal.released || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(osta{sv})"))) return;
            const char* path;
            const char* id;
            g_variant_get_child(parameters, 0, "&o", &path);
            g_variant_get_child(parameters, 1, "&s", &id);
            if (portal.shortcut != path || !g_str_equal(id, ShortcutId)) return;
            if (g_str_equal(signal, "Activated")) portal.pressed = true;
            else if (g_str_equal(signal, "Deactivated") && portal.pressed) portal.released = true;
        }, this, nullptr);
    return !stopped;
}

bool AutoTypePortal::waitForShortcut()
{
    armed = true;
    return waitFor(45000, [&] { return released; });
}

int AutoTypePortal::connectKeyboard()
{
    GUnixFDList* descriptors = nullptr;
    Variant result{g_dbus_connection_call_with_unix_fd_list_sync(bus, owner.c_str(),
        Desktop, Remote, "ConnectToEIS", g_variant_new("(oa{sv})", remote.c_str(), nullptr),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, &descriptors, nullptr, nullptr), g_variant_unref};
    int fd = -1;
    if (result && descriptors) {
        gint32 index;
        g_variant_get(result.get(), "(h)", &index);
        fd = g_unix_fd_list_get(descriptors, index, nullptr);
    }
    g_clear_object(&descriptors);
    return fd;
}
