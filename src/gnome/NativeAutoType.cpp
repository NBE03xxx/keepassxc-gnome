#include "gnome/NativeAutoType.h"
#include "gnome/AutoTypeWire.h"
#include "gnome/Sensitive.h"
#include <gdk/wayland/gdkwayland.h>
#include <unistd.h>
#include <array>
#include <csignal>
#include <string>
#include <utility>

struct NativeAutoType::Request : std::enable_shared_from_this<Request>
{
    Notify notify;
    std::string description, handle;
    GSubprocess* process = nullptr;
    GdkSurface* surface = nullptr;
    guint deadline = 0;
    bool ready = false, triggered = false, submitted = false, finished = false;

    ~Request()
    {
        releaseHandle();
        g_clear_object(&process);
    }

    void releaseHandle()
    {
        if (!handle.empty()) gdk_wayland_toplevel_drop_exported_handle(GDK_TOPLEVEL(surface), handle.c_str());
        handle.clear();
        g_clear_object(&surface);
    }

    static std::shared_ptr<Request>* keep(const std::shared_ptr<Request>& self) { return new std::shared_ptr<Request>(self); }

    void finish(Event event)
    {
        if (finished) return;
        finished = true;
        if (deadline) g_source_remove(std::exchange(deadline, 0));
        releaseHandle();
        auto callback = std::move(notify);
        if (callback) callback(event);
    }

    void cancel()
    {
        if (finished) return;
        notify = {};
        finish(Event::Interrupted);
        stopProcess();
    }

    void stopProcess()
    {
        if (!process) return;
        g_subprocess_send_signal(process, SIGTERM);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 1200, +[](gpointer data) -> gboolean {
            g_subprocess_force_exit(G_SUBPROCESS(data));
            return G_SOURCE_REMOVE;
        }, g_object_ref(process), g_object_unref);
    }

    void read()
    {
        g_input_stream_read_bytes_async(g_subprocess_get_stdout_pipe(process), 1, G_PRIORITY_DEFAULT, nullptr,
            +[](GObject* input, GAsyncResult* result, gpointer data) {
                const std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request>*>(data));
                auto& self = **holder;
                auto* bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(input), result, nullptr);
                gsize size = 0;
                const auto* value = bytes ? static_cast<const char*>(g_bytes_get_data(bytes, &size)) : nullptr;
                const char marker = size == 1 ? value[0] : '\0';
                if (bytes) g_bytes_unref(bytes);
                if (self.finished || !marker) return;
                if ((marker == AutoTypeWire::Ready || marker == AutoTypeWire::ReadyWithIme) && !self.ready) {
                    self.ready = true;
                    self.read();
                    auto notify = self.notify;
                    if (notify) notify(marker == AutoTypeWire::ReadyWithIme ? Event::ReadyWithIme : Event::Ready);
                } else if (marker == AutoTypeWire::Triggered && self.ready && !self.triggered) {
                    self.triggered = true;
                    auto notify = self.notify;
                    if (notify) notify(Event::Triggered);
                } else {
                    self.stopProcess();
                    self.finish(Event::Interrupted);
                }
            }, keep(shared_from_this()));
    }

    void spawn()
    {
        if (finished) return;
        auto* image = g_file_read_link("/proc/self/exe", nullptr);
        auto* folder = image ? g_path_get_dirname(image) : nullptr;
        auto* path = folder ? g_build_filename(folder, "keepassxc-gnome-auto-type", nullptr) : nullptr;
        const auto pid = std::to_string(::getpid());
        const auto parent = "wayland:" + handle;
        if (path) {
            const char* argv[]{path, pid.c_str(), parent.c_str(), description.c_str(), nullptr};
            process = g_subprocess_newv(argv, GSubprocessFlags(G_SUBPROCESS_FLAGS_STDIN_PIPE
                | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr);
        }
        g_free(path);
        g_free(folder);
        g_free(image);
        if (!process) {
            finish(Event::Interrupted);
            return;
        }
        g_subprocess_wait_async(process, nullptr, +[](GObject* process, GAsyncResult* result, gpointer data) {
            const std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request>*>(data));
            auto& self = **holder;
            if (!g_subprocess_wait_finish(G_SUBPROCESS(process), result, nullptr) || self.finished) return;
            if (!g_subprocess_get_if_exited(G_SUBPROCESS(process))) {
                self.finish(Event::Interrupted);
                return;
            }
            switch (g_subprocess_get_exit_status(G_SUBPROCESS(process))) {
            case 0: self.finish(self.submitted ? Event::Done : Event::Interrupted); break;
            case AutoTypeWire::PermissionOrShortcut: self.finish(Event::PermissionOrShortcut); break;
            case AutoTypeWire::UnsupportedLayout: self.finish(Event::UnsupportedLayout); break;
            case AutoTypeWire::ModifiersHeld: self.finish(Event::ModifiersHeld); break;
            case AutoTypeWire::ImeUnavailable: self.finish(Event::ImeUnavailable); break;
            case AutoTypeWire::ImeRestoreFailed: self.finish(Event::ImeRestoreFailed); break;
            default: self.finish(Event::Interrupted); break;
            }
        }, keep(shared_from_this()));
        read();
    }
};

NativeAutoType::~NativeAutoType() { cancel(); }

bool NativeAutoType::active() const { return request && !request->finished; }

bool NativeAutoType::triggered() const { return active() && request->triggered; }

void NativeAutoType::cancel()
{
    if (request) request->cancel();
    request.reset();
}

bool NativeAutoType::start(GtkWindow* parent, const char* shortcutDescription, Notify notify)
{
    if (active()) return false;
    auto* surface = gtk_native_get_surface(GTK_NATIVE(parent));
    if (!surface || !GDK_IS_WAYLAND_TOPLEVEL(surface)) return false;
    auto self = std::make_shared<Request>();
    self->notify = std::move(notify);
    self->description = shortcutDescription;
    self->surface = GDK_SURFACE(g_object_ref(surface));
    auto* data = Request::keep(self);
    if (!gdk_wayland_toplevel_export_handle(GDK_TOPLEVEL(surface),
        +[](GdkToplevel* top, const char* handle, gpointer data) {
            auto self = *static_cast<std::shared_ptr<Request>*>(data);
            if (self->finished) {
                gdk_wayland_toplevel_drop_exported_handle(top, handle);
                return;
            }
            self->handle = handle;
            if (self->deadline) g_source_remove(std::exchange(self->deadline, 0));
            self->spawn();
        }, data, +[](gpointer p) { delete static_cast<std::shared_ptr<Request>*>(p); })) {
        delete data;
        return false;
    }
    if (self->handle.empty()) {
        self->deadline = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 5, +[](gpointer data) -> gboolean {
            auto self = *static_cast<std::shared_ptr<Request>*>(data);
            self->deadline = 0;
            self->finish(Event::Interrupted);
            return G_SOURCE_REMOVE;
        }, Request::keep(self), +[](gpointer p) { delete static_cast<std::shared_ptr<Request>*>(p); });
    }
    request = std::move(self);
    return true;
}

bool NativeAutoType::send(const AutoTypeSequence& sequence)
{
    if (!triggered() || request->submitted || sequence.status != AutoTypeSequence::Status::Ready
        || sequence.steps.empty() || sequence.steps.size() > AutoTypeSequence::MaximumSteps) return false;
    std::array<std::uint32_t, AutoTypeSequence::MaximumSteps> packed{};
    const Sensitive::Erase clear{packed.data(), sizeof(packed)};
    for (std::size_t i = 0; i < sequence.steps.size(); ++i) {
        const auto& step = sequence.steps[i];
        if (step.value > AutoTypeWire::ValueMask) return false;
        packed[i] = GUINT32_TO_LE((std::uint32_t(step.kind) << AutoTypeWire::KindShift) | step.value);
    }
    const auto size = sequence.steps.size() * sizeof(packed.front());
    auto* pipe = g_subprocess_get_stdin_pipe(request->process);
    request->submitted = g_output_stream_write(pipe, packed.data(), size, nullptr, nullptr) == gssize(size);
    return request->submitted;
}
