#include "gnome/SecretClipboard.h"
#include "gnome/Sensitive.h"
#include "gnome/SessionGuard.h"
#include <cstring>
#include <memory>
#include <vector>

struct Lease {
    std::vector<char> bytes;
    gint64 expires = 0;
    bool revoked = false;
    unsigned pending = 0;
    GCancellable* cancellation = g_cancellable_new();
    void revoke() { if (!revoked) { revoked = true; g_cancellable_cancel(cancellation); Sensitive::erase(bytes.data(), bytes.size()); bytes.clear(); } }
    bool live() { if (IdleLock::bootTime() >= expires) revoke(); return !revoked; }
    ~Lease() { revoke(); g_object_unref(cancellation); }
};
struct SecretProvider { GdkContentProvider parent; std::shared_ptr<Lease>* lease; };
struct SecretProviderClass { GdkContentProviderClass parent; };
// GLib's public C type-registration macro emits C casts.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
G_DEFINE_TYPE(SecretProvider, secret_provider, GDK_TYPE_CONTENT_PROVIDER)
#pragma GCC diagnostic pop
namespace {
struct Write {
    std::vector<char> bytes;
    std::shared_ptr<Lease> lease;
    GCancellable* cancellation = g_cancellable_new();
    GCancellable* caller = nullptr;
    gulong leaseSignal = 0, callerSignal = 0;
    ~Write() {
        if (leaseSignal) g_cancellable_disconnect(lease->cancellation, leaseSignal);
        if (callerSignal) g_cancellable_disconnect(caller, callerSignal);
        g_clear_object(&caller); g_object_unref(cancellation);
        Sensitive::erase(bytes.data(), bytes.size());
    }
};
void connectCancellation(GCancellable* source, GCancellable* destination, gulong& signal) {
    signal = g_cancellable_connect(source, G_CALLBACK(+[](GCancellable*, gpointer data) { g_cancellable_cancel(G_CANCELLABLE(data)); }), g_object_ref(destination), g_object_unref);
}
void write(GdkContentProvider* provider, const char* mime, GOutputStream* stream, int priority, GCancellable* cancellation, GAsyncReadyCallback callback, gpointer data) {
    auto* task = g_task_new(provider, cancellation, callback, data);
    auto lease = *reinterpret_cast<SecretProvider*>(provider)->lease;
    if (!lease->live()) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Clipboard lease expired"); g_object_unref(task); return; }
    const bool hint = !std::strcmp(mime, "x-kde-passwordManagerHint");
    if (!hint && std::strcmp(mime, "text/plain") && std::strcmp(mime, "text/plain;charset=utf-8")) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported clipboard format"); g_object_unref(task); return;
    }
    auto* request = new Write;
    request->lease = lease;
    request->bytes = hint ? std::vector<char>{'s','e','c','r','e','t'} : lease->bytes;
    connectCancellation(lease->cancellation, request->cancellation, request->leaseSignal);
    if (cancellation) { request->caller = G_CANCELLABLE(g_object_ref(cancellation)); connectCancellation(cancellation, request->cancellation, request->callerSignal); }
    g_task_set_task_data(task, request, +[](gpointer p) { delete static_cast<Write*>(p); });
    ++lease->pending;
    g_output_stream_write_all_async(stream, request->bytes.data(), request->bytes.size(), priority, request->cancellation,
        +[](GObject* stream, GAsyncResult* result, gpointer data) {
            auto* task = G_TASK(data); GError* error = nullptr; gsize written;
            const bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(stream), result, &written, &error);
            auto* request = static_cast<Write*>(g_task_get_task_data(task));
            Sensitive::erase(request->bytes.data(), request->bytes.size()); request->bytes.clear();
            --request->lease->pending;
            if (!ok) g_task_return_error(task, error);
            else if (!request->lease->live()) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Clipboard lease revoked");
            else g_task_return_boolean(task, TRUE);
            g_object_unref(task);
        }, task);
}
}
static void secret_provider_init(SecretProvider* self) { self->lease = new std::shared_ptr<Lease>(std::make_shared<Lease>()); }
static void secret_provider_class_init(SecretProviderClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
        delete reinterpret_cast<SecretProvider*>(object)->lease;
        G_OBJECT_CLASS(secret_provider_parent_class)->finalize(object);
    };
    auto* provider = GDK_CONTENT_PROVIDER_CLASS(klass);
    provider->ref_formats = +[](GdkContentProvider*) {
        const char* formats[]{"text/plain;charset=utf-8", "text/plain", "x-kde-passwordManagerHint"}; return gdk_content_formats_new(formats, 3);
    };
    provider->ref_storable_formats = +[](GdkContentProvider*) { return gdk_content_formats_new(nullptr, 0); };
    provider->write_mime_type_async = write;
    provider->write_mime_type_finish = +[](GdkContentProvider* self, GAsyncResult* result, GError** error) -> gboolean {
        g_return_val_if_fail(g_task_is_valid(result, self), FALSE); return g_task_propagate_boolean(G_TASK(result), error);
    };
}
GdkContentProvider* secret_clipboard_new(const std::string& text, gint64 lifetime) {
    auto* provider = reinterpret_cast<SecretProvider*>(g_object_new(secret_provider_get_type(), nullptr));
    auto lease = *provider->lease; lease->bytes.assign(text.begin(), text.end()); lease->expires = IdleLock::bootTime() + std::max<gint64>(0, lifetime);
    return GDK_CONTENT_PROVIDER(provider);
}
void secret_clipboard_revoke(GdkContentProvider* provider) {
    if (provider && G_TYPE_CHECK_INSTANCE_TYPE(provider, secret_provider_get_type())) (*reinterpret_cast<SecretProvider*>(provider)->lease)->revoke();
}
bool secret_clipboard_live(GdkContentProvider* provider) {
    return provider && G_TYPE_CHECK_INSTANCE_TYPE(provider, secret_provider_get_type()) && (*reinterpret_cast<SecretProvider*>(provider)->lease)->live();
}

unsigned secret_clipboard_pending(GdkContentProvider* provider) {
    return provider && G_TYPE_CHECK_INSTANCE_TYPE(provider, secret_provider_get_type()) ? (*reinterpret_cast<SecretProvider*>(provider)->lease)->pending : 0;
}
