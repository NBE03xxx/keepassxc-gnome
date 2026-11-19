#include "gnome/NativePicker.h"
#include <gdk/wayland/gdkwayland.h>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <vector>

struct NativePicker::Request : std::enable_shared_from_this<Request> {
    Options options;
    Done done;
    std::string executable, handle;
    GSubprocess* process = nullptr;
    GdkSurface* surface = nullptr;
    std::vector<unsigned char> bytes;
    guint deadline = 0, killTimer = 0;
    bool cancelled = false, finished = false, exited = false, eof = false, exported = false;
    ~Request() { g_clear_object(&process); g_clear_object(&surface); }
    static std::shared_ptr<Request>* keep(const std::shared_ptr<Request>& self) { return new std::shared_ptr<Request>(self); }
    void finish(Status status, std::string path = {}) {
        if (finished) return;
        finished = true;
        if (deadline) { g_source_remove(deadline); deadline = 0; }
        if (killTimer) { g_source_remove(killTimer); killTimer = 0; }
        if (exported && surface) gdk_wayland_toplevel_drop_exported_handle(GDK_TOPLEVEL(surface), handle.c_str());
        exported = false; g_clear_object(&surface);
        if (done) { auto callback = std::move(done); callback(cancelled ? Status::Cancelled : status, cancelled ? std::string{} : std::move(path)); }
    }
    void complete() {
        if (finished || !exited || !eof) return;
        if (cancelled) { finish(Status::Cancelled); return; }
        if (!g_subprocess_get_successful(process) || bytes.size() < 3) { finish(Status::Failed); return; }
        const auto size = unsigned(bytes[1]) | (unsigned(bytes[2]) << 8);
        if (size > 4096 || bytes.size() != 3 + size || bytes[0] > 2) { finish(Status::Failed); return; }
        std::string path(reinterpret_cast<const char*>(bytes.data() + 3), size);
        if (bytes[0] == 0 && size && path.find('\0') == std::string::npos && g_utf8_validate(path.data(), path.size(), nullptr) && g_path_is_absolute(path.c_str())) finish(Status::Selected, std::move(path));
        else finish(bytes[0] == 1 ? Status::Cancelled : Status::Failed);
    }
    void read() {
        g_input_stream_read_bytes_async(g_subprocess_get_stdout_pipe(process), 1024, G_PRIORITY_DEFAULT, nullptr,
            +[](GObject* input, GAsyncResult* result, gpointer data) {
                std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request>*>(data)); auto self = *holder;
                GError* error = nullptr; auto* chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(input), result, &error);
                gsize size = 0; const auto* bytes = chunk ? static_cast<const unsigned char*>(g_bytes_get_data(chunk, &size)) : nullptr;
                if (!chunk || self->bytes.size() + size > 4099) {
                    self->eof = true; g_subprocess_force_exit(self->process);
                } else if (!size) self->eof = true;
                else self->bytes.insert(self->bytes.end(), bytes, bytes + size);
                if (chunk) g_bytes_unref(chunk);
                g_clear_error(&error);
                if (self->eof) self->complete(); else self->read();
            }, keep(shared_from_this()));
    }
    void cancel() {
        if (finished || cancelled) return;
        cancelled = true;
        if (process) g_subprocess_send_signal(process, SIGTERM);
        killTimer = g_timeout_add_full(G_PRIORITY_DEFAULT, 2000, +[](gpointer data) -> gboolean {
            auto self = *static_cast<std::shared_ptr<Request>*>(data); self->killTimer = 0;
            if (self->process && !self->exited) g_subprocess_force_exit(self->process);
            if (!self->process) self->finish(Status::Cancelled);
            return G_SOURCE_REMOVE;
        }, keep(shared_from_this()), +[](gpointer p) { delete static_cast<std::shared_ptr<Request>*>(p); });
    }
    void spawn(const char* parent) {
        if (finished || cancelled) { finish(Status::Cancelled); return; }
        const char* mode = options.mode == Mode::Save ? "save" : options.mode == Mode::Folder ? "folder" : options.mode == Mode::KeyFile ? "keyfile" : "open";
        const auto pid = std::to_string(::getpid());
        const char* args[]{executable.c_str(), mode, options.title.c_str(), options.folder.c_str(), options.name.c_str(), parent, pid.c_str(), "v1", nullptr};
        process = g_subprocess_newv(args, GSubprocessFlags(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr);
        if (!process) { finish(Status::Failed); return; }
        g_subprocess_wait_async(process, nullptr, +[](GObject* process, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request>*>(data)); auto self = *holder;
            g_subprocess_wait_finish(G_SUBPROCESS(process), result, nullptr); self->exited = true; self->complete();
        }, keep(shared_from_this()));
        read();
    }
};
NativePicker::NativePicker(std::string executable) : executable(std::move(executable)) {
    if (this->executable.empty()) {
        auto* self = g_file_read_link("/proc/self/exe", nullptr); auto* folder = self ? g_path_get_dirname(self) : nullptr;
        auto* path = folder ? g_build_filename(folder, "keepassxc-gnome-picker", nullptr) : nullptr;
        if (path) this->executable = path;
        g_free(path); g_free(folder); g_free(self);
    }
}
NativePicker::~NativePicker() { cancel(); }
bool NativePicker::active() const { return request && !request->finished; }
void NativePicker::cancel() { if (request) request->cancel(); }
bool NativePicker::choose(GtkWindow* parent, Options options, Done done) {
    if (active()) return false;
    request = std::make_shared<Request>(); auto self = request;
    self->options = std::move(options); self->done = std::move(done); self->executable = executable;
    self->deadline = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 600, +[](gpointer data) -> gboolean {
        auto self = *static_cast<std::shared_ptr<Request>*>(data); self->deadline = 0; self->cancel(); return G_SOURCE_REMOVE;
    }, Request::keep(self), +[](gpointer p) { delete static_cast<std::shared_ptr<Request>*>(p); });
    auto* surface = parent ? gtk_native_get_surface(GTK_NATIVE(parent)) : nullptr;
    if (surface && GDK_IS_WAYLAND_TOPLEVEL(surface)) {
        self->surface = GDK_SURFACE(g_object_ref(surface));
        auto* data = Request::keep(self);
        if (gdk_wayland_toplevel_export_handle(GDK_TOPLEVEL(surface), +[](GdkToplevel* top, const char* handle, gpointer data) {
            auto self = *static_cast<std::shared_ptr<Request>*>(data);
            if (self->finished) { gdk_wayland_toplevel_drop_exported_handle(top, handle); return; }
            self->exported = true; self->handle = handle; const auto parent = std::string("wayland:") + handle; self->spawn(parent.c_str());
        }, data, +[](gpointer p) { delete static_cast<std::shared_ptr<Request>*>(p); })) return true;
        delete data; g_clear_object(&self->surface);
    }
    self->spawn(""); return true;
}
