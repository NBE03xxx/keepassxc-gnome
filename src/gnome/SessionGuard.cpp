#include "gnome/SessionGuard.h"
#include <array>
#include <cstring>
#include <string>
#include <unistd.h>

struct SessionGuard::State : std::enable_shared_from_this<State> {
    struct Endpoint { GDBusProxy* proxy = nullptr; bool owned = false, active = false, known = false, failed = false, initialized = false, observed = false, unsupported = false; unsigned revision = 0; std::string ownerName; };
    std::array<Endpoint, 3> endpoints;
    GCancellable* cancellation = g_cancellable_new();
    std::function<void()> lock;
    std::function<void(bool)> availability;
    bool alive = true;
    int sleepDelay = -1;
    unsigned sleepEpoch = 0;
    bool acquiring = false;
    gint64 nextAttempt = 0;
    void releaseDelay() { if (sleepDelay >= 0) { ::close(sleepDelay); sleepDelay = -1; } }
    ~State() { releaseDelay(); for (auto& endpoint : endpoints) g_clear_object(&endpoint.proxy); g_object_unref(cancellation); }
    bool nativeStateKnown() const { return endpoints[0].owned && endpoints[0].known && !endpoints[0].failed; }
    bool redundantUnsupported(unsigned i) const { return i == 1 && endpoints[i].unsupported && nativeStateKnown(); }
    void available() {
        bool failed = false;
        for (unsigned i = 0; i < endpoints.size(); ++i) if (!redundantUnsupported(i)) failed = failed || endpoints[i].failed;
        if (alive) availability(!failed && ((endpoints[0].owned && endpoints[0].known) || (endpoints[1].owned && endpoints[1].known)) && endpoints[2].owned && endpoints[2].known && sleepDelay >= 0);
    }
    struct Context { std::shared_ptr<State> state; unsigned index, revision = 0; std::string owner; };
    void acquireDelay() {
        if (!alive || !endpoints[2].proxy || !endpoints[2].owned || endpoints[2].active || sleepDelay >= 0 || acquiring || g_get_monotonic_time() < nextAttempt) return;
        auto* owner = g_dbus_proxy_get_name_owner(endpoints[2].proxy); if (!owner) return;
        acquiring = true; nextAttempt = g_get_monotonic_time() + 5000000;
        auto* context = new Context{shared_from_this(), 2, sleepEpoch, owner};
        g_dbus_connection_call_with_unix_fd_list(g_dbus_proxy_get_connection(endpoints[2].proxy), owner,
            "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
            g_variant_new("(ssss)", "sleep", "KeePassXC GNOME", g_dgettext("keepassxc-gnome", "Lock database before sleep"), "delay"),
            G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, cancellation,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Context> context(static_cast<Context*>(data));
                GError* error = nullptr; GUnixFDList* descriptors = nullptr;
                auto* reply = g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source), &descriptors, result, &error);
                auto& s = *context->state;
                if (context->revision == s.sleepEpoch) s.acquiring = false;
                auto* owner = s.endpoints[2].proxy ? g_dbus_proxy_get_name_owner(s.endpoints[2].proxy) : nullptr;
                if (s.alive && reply && descriptors && owner && context->owner == owner && context->revision == s.sleepEpoch && !s.endpoints[2].active) {
                    gint handle; g_variant_get(reply, "(h)", &handle); int fd = g_unix_fd_list_get(descriptors, handle, &error);
                    if (fd >= 0) { s.releaseDelay(); s.sleepDelay = fd; s.nextAttempt = 0; }
                }
                if (s.alive) {
                    if (context->revision == s.sleepEpoch && s.sleepDelay < 0 && !s.endpoints[2].active) s.lock();
                    s.available();
                }
                g_free(owner); if (reply) g_variant_unref(reply); g_clear_object(&descriptors); g_clear_error(&error);
            }, context);
        g_free(owner);
    }
    void owner(unsigned index, bool retry = false) {
        auto& endpoint = endpoints[index];
        auto* owner = g_dbus_proxy_get_name_owner(endpoint.proxy);
        const std::string ownerName = owner ? owner : "";
        if (!retry && endpoint.observed && endpoint.ownerName == ownerName) { g_free(owner); return; }
        endpoint.observed = true; endpoint.ownerName = ownerName; endpoint.unsupported = false;
        const bool wasOwned = endpoint.owned;
        if (index == 2) { ++sleepEpoch; acquiring = false; nextAttempt = 0; releaseDelay(); }
        endpoint.owned = owner != nullptr; endpoint.active = wasOwned; endpoint.known = false;
        endpoint.failed = wasOwned && !owner; ++endpoint.revision;
        if (wasOwned) lock(); // replacement/loss is fail-closed, never an unlock
        available();
        if (owner) {
            auto* context = new Context{shared_from_this(), index, endpoint.revision, owner};
            g_dbus_connection_call(g_dbus_proxy_get_connection(endpoint.proxy), owner,
                g_dbus_proxy_get_object_path(endpoint.proxy), index == 2 ? "org.freedesktop.DBus.Properties" : g_dbus_proxy_get_interface_name(endpoint.proxy),
                index == 2 ? "Get" : "GetActive", index == 2 ? g_variant_new("(ss)", "org.freedesktop.login1.Manager", "PreparingForSleep") : nullptr,
                index == 2 ? G_VARIANT_TYPE("(v)") : G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, cancellation,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    std::unique_ptr<Context> context(static_cast<Context*>(data));
                    GError* error = nullptr;
                    auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                    auto& s = *context->state; auto& e = s.endpoints[context->index];
                    auto* currentOwner = e.proxy ? g_dbus_proxy_get_name_owner(e.proxy) : nullptr;
                    if (s.alive && reply && currentOwner && context->owner == currentOwner && e.revision == context->revision) {
                        gboolean active = TRUE; bool valid = true;
                        if (context->index == 2) {
                            GVariant* value; g_variant_get(reply, "(v)", &value);
                            valid = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN);
                            if (valid) active = g_variant_get_boolean(value);
                            g_variant_unref(value);
                        } else g_variant_get(reply, "(b)", &active);
                        e.active = active; e.known = valid; e.failed = !valid;
                        if (active) s.lock();
                        if (context->index == 2 && !active) s.acquireDelay();
                        s.available();
                    } else if (s.alive && !reply && e.revision == context->revision) {
                        // A failed initial query must not silently certify an
                        // unlocked desktop; signal monitoring remains active.
                        // org.freedesktop.ScreenSaver standardizes idle
                        // inhibition, not GetActive. GNOME 50 explicitly
                        // rejects that extension. Only a verified native
                        // GNOME state can make this redundant source optional;
                        // access errors/timeouts remain fail-closed.
                        auto* remote = error ? g_dbus_error_get_remote_error(error) : nullptr;
                        e.unsupported = context->index == 1 && remote &&
                            (!std::strcmp(remote, "org.freedesktop.DBus.Error.NotSupported")
                             || !std::strcmp(remote, "org.freedesktop.DBus.Error.UnknownMethod")
                             || !std::strcmp(remote, "org.freedesktop.DBus.Error.UnknownInterface"));
                        g_free(remote);
                        e.active = true; e.failed = true; s.lock(); s.available();
                    }
                    g_free(currentOwner); if (reply) g_variant_unref(reply); g_clear_error(&error);
                }, context);
        }
        g_free(owner);
    }
    void start(unsigned index, GBusType bus, const char* name, const char* path, const char* interface) {
        g_dbus_proxy_new_for_bus(bus, GDBusProxyFlags(G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES),
            nullptr, name, path, interface, cancellation, +[](GObject*, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Context> context(static_cast<Context*>(data));
                auto& s = *context->state; GError* error = nullptr;
                auto* proxy = g_dbus_proxy_new_for_bus_finish(result, &error);
                if (!s.alive) { g_clear_object(&proxy); g_clear_error(&error); return; }
                s.endpoints[context->index].initialized = true;
                if (!proxy) { s.available(); g_clear_error(&error); return; }
                s.endpoints[context->index].proxy = proxy;
                // The proxy owns weak callback contexts, avoiding a State/proxy cycle.
                struct Watch { std::weak_ptr<State> state; unsigned index; };
                auto* watch = new Watch{context->state, context->index};
                g_signal_connect_data(proxy, "notify::g-name-owner", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                    auto* watch = static_cast<Watch*>(data); if (auto s = watch->state.lock(); s && s->alive) s->owner(watch->index);
                }), watch, +[](gpointer p, GClosure*) { delete static_cast<Watch*>(p); }, G_CONNECT_DEFAULT);
                auto* signals = new Watch{context->state, context->index};
                g_signal_connect_data(proxy, "g-signal", G_CALLBACK(+[](GDBusProxy* proxy, const char* sender, const char* signal, GVariant* parameters, gpointer data) {
                    auto* watch = static_cast<Watch*>(data); auto s = watch->state.lock(); if (!s || !s->alive) return;
                    auto* owner = g_dbus_proxy_get_name_owner(proxy);
                    const bool trusted = sender && owner && !std::strcmp(sender, owner); g_free(owner);
                    if (!trusted || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) return;
                    const bool expected = watch->index == 2 ? !std::strcmp(signal, "PrepareForSleep") : !std::strcmp(signal, "ActiveChanged");
                    if (!expected) return;
                    gboolean active; g_variant_get(parameters, "(b)", &active);
                    auto& e = s->endpoints[watch->index]; ++e.revision; e.active = active; e.known = true; e.failed = false; s->available();
                    if (watch->index == 2) { ++s->sleepEpoch; s->acquiring = false; s->nextAttempt = 0; if (!active) { s->releaseDelay(); s->acquireDelay(); } }
                    if (active || watch->index == 2) s->lock(); // resume also stays locked
                }), signals, +[](gpointer p, GClosure*) { delete static_cast<Watch*>(p); }, G_CONNECT_DEFAULT);
                s.owner(context->index);
            }, new Context{shared_from_this(), index, 0, {}});
    }
};
SessionGuard::SessionGuard(std::function<void()> lock, std::function<void(bool)> availability) : state(std::make_shared<State>()) {
    state->lock = std::move(lock); state->availability = std::move(availability);
    state->start(0, G_BUS_TYPE_SESSION, "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver");
    state->start(1, G_BUS_TYPE_SESSION, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver");
    state->start(2, G_BUS_TYPE_SYSTEM, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager");
}
SessionGuard::~SessionGuard() { state->alive = false; state->releaseDelay(); g_cancellable_cancel(state->cancellation); state->lock = {}; state->availability = {}; }
bool SessionGuard::settled() const {
    for (const auto& e : state->endpoints) if (!e.initialized || (e.owned && !e.known && !e.failed)) return false;
    return true;
}
bool SessionGuard::blocked() const {
    if (!settled()) return true;
    for (unsigned i = 0; i < state->endpoints.size(); ++i)
        if (state->endpoints[i].active && !state->redundantUnsupported(i)) return true;
    return state->endpoints[2].owned && state->sleepDelay < 0;
}

void SessionGuard::releaseSleepDelay() {
    if (state->endpoints[2].active) state->releaseDelay();
}

void SessionGuard::refresh() {
    for (unsigned i = 0; i < 3; ++i) if (state->endpoints[i].owned && state->endpoints[i].failed && !state->endpoints[i].unsupported) state->owner(i, true);
    state->acquireDelay();
}
