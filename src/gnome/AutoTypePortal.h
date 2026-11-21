#pragma once

#include <gio/gio.h>
#include <functional>
#include <memory>
#include <string>

// Used only in the short-lived Auto-Type helper. Its D-Bus connection owns all
// requests and sessions, and never handles clipboard or screen sharing.
class AutoTypePortal
{
public:
    using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
    AutoTypePortal() = default;
    ~AutoTypePortal();

    bool connect();
    bool prepare(const char* parent, const char* shortcutDescription);
    bool waitForShortcut();
    int connectKeyboard();
    bool imeAvailable() const { return !imeOwner.empty(); }
    bool imeFailed() const { return imeRegistrationFailed; }
    bool beginIme();
    bool endIme();
    bool waitFor(unsigned milliseconds, const std::function<bool()>& until) const;
    void cancel() { stopped = true; }
    bool cancelled() const { return stopped; }

private:
    Variant call(const std::string& path, const char* interface, const char* method,
                 GVariant* args, const GVariantType* type) const;
    Variant request(const char* interface, const char* method, unsigned milliseconds,
                    const std::function<GVariant*(const char*)>& args) const;
    bool createSession(const char* interface, std::string& session);
    bool connectIme();
    bool callIme(const char* method, GError** error = nullptr) const;
    GDBusConnection* bus = nullptr;
    std::string owner, sender, shortcut, remote, imeOwner;
    bool imeRegistrationFailed = false, imeActive = false;
    bool stopped = false, armed = false, pressed = false, released = false;
};
