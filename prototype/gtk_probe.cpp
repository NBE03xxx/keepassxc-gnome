#include "CoreSession.h"

#include <adwaita.h>
#include <gdk/wayland/gdkwayland.h>
#include <glib-unix.h>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <utility>

namespace
{
enum class SmokeTest { None, Expiry, Replacement, Desktop, PickerLifecycle, SessionLock };

constexpr auto ScreenLockMessage = "Locked after the GNOME screen locked.";
constexpr auto SleepLockMessage = "Locked on the pre-suspend notification.";
constexpr auto LoginName = "org.freedesktop.login1";
constexpr auto LoginPath = "/org/freedesktop/login1";
constexpr auto LoginManager = "org.freedesktop.login1.Manager";

struct LockSignal
{
    void disconnect()
    {
        if (connection) {
            g_dbus_connection_signal_unsubscribe(connection, subscription);
            g_clear_object(&connection);
        }
    }

    GDBusConnection* connection = nullptr;
    guint subscription = 0;
};

struct SleepDelay
{
    ~SleepDelay() { release(); }

    void release()
    {
        ++generation;
        requesting = false;
        g_clear_fd(&fd, nullptr);
    }

    static void acquire(const std::shared_ptr<SleepDelay>& delay, GDBusConnection* bus)
    {
        if (!bus || delay->fd >= 0 || delay->requesting) {
            return;
        }
        delay->requesting = true;
        using Request = std::pair<std::weak_ptr<SleepDelay>, unsigned>;
        g_dbus_connection_call_with_unix_fd_list(
            bus, LoginName, LoginPath, LoginManager, "Inhibit",
            g_variant_new("(ssss)", "sleep", "KeePassXC GTK fixture probe", "Lock the fixture before sleep", "delay"),
            G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) {
                const std::unique_ptr<Request> request(static_cast<Request*>(data));
                const auto& [weakDelay, generation] = *request;
                GUnixFDList* descriptors = nullptr;
                GError* error = nullptr;
                auto* reply = g_dbus_connection_call_with_unix_fd_list_finish(
                    G_DBUS_CONNECTION(source), &descriptors, result, &error);
                if (const auto delay = weakDelay.lock(); delay && generation == delay->generation) {
                    delay->requesting = false;
                    if (reply && descriptors) {
                        gint handle = -1;
                        g_variant_get(reply, "(h)", &handle);
                        delay->fd = g_unix_fd_list_get(descriptors, handle, &error);
                    }
                    if (error) {
                        g_debug("Could not acquire logind sleep delay: %s", error->message);
                    }
                }
                g_clear_error(&error);
                g_clear_pointer(&reply, g_variant_unref);
                g_clear_object(&descriptors);
            }, new Request(delay, delay->generation));
    }

    int fd = -1;
    unsigned generation = 0;
    bool requesting = false;
};

class CoreWorker
{
public:
    CoreWorker()
    {
        receiver->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, receiver, &QObject::deleteLater);
        thread.start();
    }

    ~CoreWorker()
    {
        QMetaObject::invokeMethod(receiver, [this] { session.reset(); }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }

    template<typename Task, typename Done>
    void run(Task task, Done done)
    {
        QMetaObject::invokeMethod(receiver, [this, task = std::move(task), done = std::move(done)]() mutable {
            if (!session) {
                session = std::make_unique<CoreSession>();
            }
            auto result = task(*session);
            auto* reply = new std::function<void()>([done = std::move(done), result = std::move(result)]() mutable {
                done(std::move(result));
            });
            auto* source = g_idle_source_new();
            g_source_set_callback(source, [](gpointer data) -> gboolean {
                (*static_cast<std::function<void()>*>(data))();
                return G_SOURCE_REMOVE;
            }, reply, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
            g_source_attach(source, g_main_context_default());
            g_source_unref(source);
        }, Qt::QueuedConnection);
    }

private:
    QThread thread;
    QObject* receiver = new QObject();
    std::unique_ptr<CoreSession> session;
};

class Window
{
public:
    Window(AdwApplication* application, SmokeTest smokeTest)
        : app(application), smoke(smokeTest)
    {
    }

    ~Window()
    {
        if (loginWatch) {
            g_bus_unwatch_name(loginWatch);
        }
        screenLockSignal.disconnect();
        sleepSignal.disconnect();
        if (smokeTimer) {
            g_source_remove(smokeTimer);
        }
        clearClipboard();
        if (clipboard) {
            g_signal_handlers_disconnect_by_data(clipboard, this);
        }
        g_clear_object(&clipboard);
        g_clear_object(&appearanceSettings);
        releasePickerHandle();
        if (window) {
            gtk_window_destroy(window);
        }
    }

    void activate()
    {
        if (window) {
            gtk_window_present(window);
            return;
        }

        window = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
        gtk_window_set_title(window, "KeePassXC GTK — fixture probe");
        gtk_window_set_default_size(window, 520, 320);
        g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
            auto& self = *static_cast<Window*>(data);
            self.closing = true;
            self.conceal();
            self.stopWhenIdle();
            return TRUE;
        }), this);

        auto* view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
        adw_toolbar_view_add_top_bar(view, adw_header_bar_new());
        adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), GTK_WIDGET(view));
        auto* box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 10));
        gtk_widget_set_margin_start(GTK_WIDGET(box), 16);
        gtk_widget_set_margin_end(GTK_WIDGET(box), 16);
        gtk_widget_set_margin_top(GTK_WIDGET(box), 16);
        gtk_widget_set_margin_bottom(GTK_WIDGET(box), 16);
        adw_toolbar_view_set_content(view, GTK_WIDGET(box));

        status = GTK_LABEL(gtk_label_new("Preparing a synthetic database…"));
        gtk_label_set_wrap(status, TRUE);
        gtk_box_append(box, GTK_WIDGET(status));
        chooseButton = button(box, "Choose a generated fixture", &Window::choose);
        password = GTK_EDITABLE(gtk_password_entry_new());
        g_object_set(password, "placeholder-text", "Fixture password: phase2-demo", nullptr);
        gtk_box_append(box, GTK_WIDGET(password));
        unlockButton = button(box, "Unlock", &Window::unlock);

        titles = gtk_string_list_new(nullptr);
        selection = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(titles), nullptr));
        gtk_box_append(box, GTK_WIDGET(selection));
        title = GTK_EDITABLE(gtk_entry_new());
        gtk_entry_set_placeholder_text(GTK_ENTRY(title), "New title for the selected entry");
        gtk_box_append(box, GTK_WIDGET(title));
        renameButton = button(box, "Apply title", &Window::rename);
        saveButton = button(box, "Save a separate fixture copy", &Window::save);
        copyButton = button(box, "Copy fixture password for 5 seconds", &Window::copy);
        lockButton = button(box, "Lock", &Window::lock);

        followGnomeAppearance();
        clipboard = GDK_CLIPBOARD(g_object_ref(gtk_widget_get_clipboard(GTK_WIDGET(window))));
        g_signal_connect(clipboard, "changed", G_CALLBACK(+[](GdkClipboard* source, gpointer data) {
            auto& self = *static_cast<Window*>(data);
            if (self.copied && gdk_clipboard_get_content(source) != self.copied) {
                self.clearClipboard();
            }
        }), this);
        if (smoke == SmokeTest::SessionLock) {
            auto* keys = gtk_event_controller_key_new();
            gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
            g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint,
                                                                GdkModifierType, gpointer data) -> gboolean {
                if (key == GDK_KEY_Shift_L) {
                    static_cast<Window*>(data)->testClipboardInput = true;
                }
                return FALSE;
            }), this);
            gtk_widget_add_controller(GTK_WIDGET(window), keys);
        }
        subscribeLockSignal(screenLockSignal, G_BUS_TYPE_SESSION, "org.gnome.ScreenSaver",
                            "org.gnome.ScreenSaver", "ActiveChanged", "/org/gnome/ScreenSaver");
        subscribeLockSignal(sleepSignal, G_BUS_TYPE_SYSTEM, LoginName, LoginManager, "PrepareForSleep", LoginPath);
        if (sleepSignal.connection) {
            loginWatch = g_bus_watch_name_on_connection(
                sleepSignal.connection, LoginName, G_BUS_NAME_WATCHER_FLAGS_NONE,
                [](GDBusConnection*, const gchar*, const gchar*, gpointer data) {
                    auto& self = *static_cast<Window*>(data);
                    self.loginAvailable = true;
                    self.finishSleepPreparation();
                },
                [](GDBusConnection*, const gchar*, gpointer data) {
                    auto& self = *static_cast<Window*>(data);
                    self.loginAvailable = false;
                    self.sleepDelay->release();
                    self.prepareForSleep(false);
                }, this, nullptr);
        }
        gtk_window_present(window);
        if (!directory.isValid()) {
            finish(1, "Cannot create fixture directory");
            return;
        }
        selectedFile = directory.filePath(QStringLiteral("fixture.kdbx")).toStdString();
        submit([file = selectedFile](CoreSession&) { return CoreSession::createFixture(file, FixtureFormat::Kdbx4); },
               [this](bool ok) {
                   ready = ok;
                   if (ok) {
                       files.push_back(selectedFile);
                   }
                   setStatus(ok ? "Synthetic fixture ready. Unlock with phase2-demo." : "Could not create fixture.");
                   if (smoke != SmokeTest::None && !ok) {
                       finish(1, "Fixture creation");
                   }
               });
        if (smoke != SmokeTest::None) {
            smokeTimer = g_timeout_add(25, [](gpointer data) -> gboolean {
                auto& self = *static_cast<Window*>(data);
                const auto keep = self.smokeTick();
                if (keep == G_SOURCE_REMOVE) {
                    self.smokeTimer = 0;
                }
                return keep;
            }, this);
        }
    }

    int exitStatus = 0;

private:
    void subscribeLockSignal(LockSignal& signal, GBusType bus, const char* sender, const char* interface,
                             const char* member, const char* path)
    {
        signal.connection = g_bus_get_sync(bus, nullptr, nullptr);
        if (!signal.connection) {
            return;
        }
        signal.subscription = g_dbus_connection_signal_subscribe(
            signal.connection, sender, interface, member, path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar* interface, const gchar*, GVariant* parameters,
               gpointer data) {
                if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) {
                    return;
                }
                gboolean active = FALSE;
                g_variant_get(parameters, "(b)", &active);
                auto& self = *static_cast<Window*>(data);
                if (g_str_equal(interface, LoginManager)) {
                    self.prepareForSleep(active);
                } else if (active) {
                    self.lockForDesktopEvent(ScreenLockMessage);
                }
            }, this, nullptr);
    }

    void applyGnomeAppearance()
    {
        auto* preference = g_settings_get_string(appearanceSettings, "color-scheme");
        const auto scheme = g_str_equal(preference, "prefer-dark") ? ADW_COLOR_SCHEME_PREFER_DARK
                                                                  : ADW_COLOR_SCHEME_DEFAULT;
        adw_style_manager_set_color_scheme(adw_application_get_style_manager(app), scheme);
        g_free(preference);
    }

    void followGnomeAppearance()
    {
        if (adw_style_manager_get_system_supports_color_schemes(adw_application_get_style_manager(app))) {
            return;
        }
        auto* source = g_settings_schema_source_get_default();
        auto* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
        if (!schema) {
            return;
        }
        if (g_settings_schema_has_key(schema, "color-scheme")) {
            appearanceSettings = g_settings_new_full(schema, nullptr, nullptr);
            g_signal_connect(appearanceSettings, "changed::color-scheme", G_CALLBACK(+[](GSettings*, gchar*, gpointer data) {
                static_cast<Window*>(data)->applyGnomeAppearance();
            }), this);
            applyGnomeAppearance();
        }
        g_settings_schema_unref(schema);
    }

    GtkWidget* button(GtkBox* box, const char* label, void (Window::*action)())
    {
        auto* widget = gtk_button_new_with_label(label);
        using Action = std::pair<Window*, void (Window::*)()>;
        g_signal_connect_data(widget, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* callback = static_cast<Action*>(data);
            (callback->first->*callback->second)();
        }), new Action(this, action), [](gpointer data, GClosure*) {
            delete static_cast<Action*>(data);
        }, G_CONNECT_DEFAULT);
        gtk_box_append(box, widget);
        return widget;
    }

    template<typename Task, typename Done, typename Record = std::identity>
    void submit(Task task, Done done, Record record = {})
    {
        busy = true;
        updateControls();
        worker.run(std::move(task), [this, done = std::move(done), record = std::move(record)](auto result) mutable {
            busy = false;
            if (closing) {
                g_application_quit(G_APPLICATION(app));
                return;
            }
            // Keep successful copies selectable when a pending lock hides the operation's reply.
            static_cast<void>(record(std::as_const(result)));
            if (lockPending) {
                lockPending = false;
                completeLock();
                return;
            }
            done(std::move(result));
            updateControls();
        });
    }

    void setStatus(const char* message)
    {
        gtk_label_set_text(status, message);
    }

    void updateControls()
    {
        const auto idle = ready && !sleeping && !busy && !chooser && !exportingPickerHandle;
        gtk_widget_set_sensitive(chooseButton, idle && !unlocked);
        gtk_widget_set_sensitive(unlockButton, idle && !unlocked);
        gtk_widget_set_sensitive(GTK_WIDGET(password), idle && !unlocked);
        gtk_widget_set_sensitive(GTK_WIDGET(selection), idle && unlocked);
        gtk_widget_set_sensitive(GTK_WIDGET(title), idle && unlocked);
        gtk_widget_set_sensitive(renameButton, idle && unlocked);
        gtk_widget_set_sensitive(saveButton, idle && unlocked);
        gtk_widget_set_sensitive(copyButton, idle && unlocked);
        gtk_widget_set_sensitive(lockButton, unlocked || unlocking);
    }

    void choose()
    {
        if (sleeping || busy || unlocked || chooser || exportingPickerHandle) {
            return;
        }
        pickerInvalidated = false;
        auto* surface = gtk_native_get_surface(GTK_NATIVE(window));
        if (GDK_IS_WAYLAND_TOPLEVEL(surface)) {
            exportingPickerHandle = true;
            if (gdk_wayland_toplevel_export_handle(GDK_TOPLEVEL(surface), [](GdkToplevel* toplevel, const char* handle,
                                                                            gpointer data) {
                auto& self = *static_cast<Window*>(data);
                self.exportingPickerHandle = false;
                if (handle && *handle) {
                    self.pickerToplevel = GDK_TOPLEVEL(g_object_ref(toplevel));
                    self.pickerHandle = handle;
                }
                if (self.closing) {
                    self.releasePickerHandle();
                    self.stopWhenIdle();
                } else if (self.pickerInvalidated) {
                    self.releasePickerHandle();
                    self.updateControls();
                } else {
                    self.startPicker();
                }
            }, this, nullptr)) {
                updateControls();
                return;
            }
            exportingPickerHandle = false;
        }
        startPicker();
    }

    void startPicker()
    {
        // Exec a picker process so the database process can stay undumpable.
        const auto program = QCoreApplication::applicationDirPath().toUtf8() + "/phase2_file_picker";
        const auto folder = directory.path().toUtf8();
        const auto parent = pickerHandle.empty() ? std::string{} : "wayland:" + pickerHandle;
        chooser = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, nullptr, program.constData(),
                                   folder.constData(), parent.c_str(), nullptr);
        if (!chooser) {
            releasePickerHandle();
            updateControls();
            setStatus("Could not start the fixture picker.");
            return;
        }
        updateControls();
        g_subprocess_communicate_async(chooser, nullptr, nullptr, [](GObject* source, GAsyncResult* result, gpointer data) {
            auto& self = *static_cast<Window*>(data);
            auto* process = G_SUBPROCESS(source);
            GBytes* output = nullptr;
            const bool completed = g_subprocess_communicate_finish(process, result, &output, nullptr, nullptr);
            const bool cancelled = pickerCancelled(process, completed);
            if (!self.closing && !self.pickerInvalidated) {
                self.acceptPickerResult(process, completed, cancelled, output);
            }
            g_clear_pointer(&output, g_bytes_unref);
            g_clear_object(&self.chooser);
            self.releasePickerHandle();
            if (self.closing) {
                if (self.smoke == SmokeTest::PickerLifecycle && self.smokeStep == 3 && self.exitStatus == 0) {
                    self.finish(cancelled ? 0 : 1, cancelled ? "picker request closed with parent" : "picker shutdown");
                } else {
                    self.stopWhenIdle();
                }
            } else {
                self.updateControls();
            }
        }, this);
    }

    void releasePickerHandle()
    {
        if (pickerToplevel) {
            gdk_wayland_toplevel_drop_exported_handle(pickerToplevel, pickerHandle.c_str());
            g_clear_object(&pickerToplevel);
            pickerHandle.clear();
        }
    }

    void stopWhenIdle()
    {
        if (chooser) {
            g_subprocess_send_signal(chooser, SIGTERM);
        } else if (!busy && !exportingPickerHandle) {
            g_application_quit(G_APPLICATION(app));
        }
    }

    static bool pickerCancelled(GSubprocess* process, bool completed)
    {
        return completed && g_subprocess_get_if_exited(process) && g_subprocess_get_exit_status(process) == 2;
    }

    void acceptPickerResult(GSubprocess* process, bool completed, bool cancelled, GBytes* output)
    {
        if (cancelled) {
            setStatus("Fixture selection cancelled.");
            return;
        }
        if (!completed || !output || !g_subprocess_get_successful(process)) {
            setStatus("The fixture picker did not return a local file.");
            return;
        }
        gsize size = 0;
        const auto* data = static_cast<const char*>(g_bytes_get_data(output, &size));
        const auto path = size == 0 ? std::string{} : std::string(data, size);
        if (std::ranges::find(files, path) == files.end()) {
            setStatus("Choose a fixture generated by this window.");
            return;
        }
        selectedFile = path;
        setStatus("Generated fixture selected.");
    }

    void unlock()
    {
        if (sleeping || busy || unlocked) {
            return;
        }
        std::string input(gtk_editable_get_text(password));
        gtk_editable_set_text(password, "");
        unlocking = true;
        submit([file = selectedFile, input = std::move(input)](CoreSession& session) {
                   session.unlock(file, input);
                   return session.entries();
               }, [this](std::vector<EntrySummary> rows) {
                   unlocking = false;
                   unlocked = !rows.empty();
                   display(std::move(rows));
                   setStatus(unlocked ? "Synthetic database unlocked." : "Could not unlock the fixture.");
               });
    }

    void display(std::vector<EntrySummary> rows)
    {
        const auto currentId = selectedId();
        gtk_string_list_splice(titles, 0, g_list_model_get_n_items(G_LIST_MODEL(titles)), nullptr);
        entries = std::move(rows);
        for (const auto& row : entries) {
            gtk_string_list_append(titles, row.title.c_str());
        }
        const auto current = std::ranges::find(entries, currentId, &EntrySummary::id);
        const auto index = current == entries.end() ? 0 : static_cast<guint>(current - entries.begin());
        gtk_drop_down_set_selected(selection, entries.empty() ? GTK_INVALID_LIST_POSITION : index);
    }

    std::string selectedId() const
    {
        const auto index = gtk_drop_down_get_selected(selection);
        return index < entries.size() ? entries[index].id : std::string{};
    }

    void rename()
    {
        if (busy || !unlocked) {
            return;
        }
        submit([id = selectedId(), newTitle = std::string(gtk_editable_get_text(title))](CoreSession& session) {
                   const bool renamed = session.rename(id, newTitle);
                   return std::pair{renamed, session.entries()};
               }, [this](std::pair<bool, std::vector<EntrySummary>> result) {
                   if (result.first) {
                       display(std::move(result.second));
                       gtk_editable_set_text(title, "");
                   } else {
                       setStatus("Select an entry and enter a title.");
                   }
               });
    }

    void save()
    {
        if (busy || !unlocked) {
            return;
        }
        const auto path = directory.filePath(QStringLiteral("copy-%1.kdbx").arg(nextCopyNumber++)).toStdString();
        submit([path](CoreSession& session) { return session.saveCopy(path); },
               [this](bool ok) { setStatus(ok ? "Encrypted fixture copy saved." : "Copy could not be saved."); },
               [this, path](bool ok) {
                   if (ok) {
                       files.push_back(path);
                   }
               });
    }

    void copy()
    {
        if (busy || !unlocked) {
            return;
        }
        submit([id = selectedId()](CoreSession& session) { return session.copyPassword(id); },
               [this](const std::string& text) {
                   clearClipboard();
                   copyExpired = false;
                   if (text.empty()) {
                       return;
                   }
                   auto* provider = gdk_content_provider_new_typed(G_TYPE_STRING, text.c_str());
                   if (gdk_clipboard_set_content(clipboard, provider)) {
                       copied = provider;
                       copyTimer = g_timeout_add_seconds(5, [](gpointer data) -> gboolean {
                           auto& self = *static_cast<Window*>(data);
                           self.copyTimer = 0;
                           self.copyExpired = true;
                           self.clearClipboard();
                           return G_SOURCE_REMOVE;
                       }, this);
                       setStatus("Synthetic value copied for 5 seconds.");
                   } else {
                       g_object_unref(provider);
                       setStatus("The clipboard could not accept the synthetic value.");
                   }
               });
    }

    void clearClipboard()
    {
        if (copyTimer) {
            g_source_remove(copyTimer);
            copyTimer = 0;
        }
        auto* provider = std::exchange(copied, nullptr);
        if (provider && gdk_clipboard_is_local(clipboard) && gdk_clipboard_get_content(clipboard) == provider) {
            gdk_clipboard_set_content(clipboard, nullptr);
        }
        g_clear_object(&provider);
    }

    void conceal()
    {
        unlocked = false;
        unlocking = false;
        display({});
        gtk_editable_set_text(title, "");
        gtk_editable_set_text(password, "");
        clearClipboard();
    }

    void lock()
    {
        lockFixture("Locked. Unsaved fixture changes were discarded.");
    }

    void prepareForSleep(bool active)
    {
        if (closing) {
            return;
        }
        sleeping = active;
        if (active) {
            sleepPreparationPending = true;
            lockForDesktopEvent(SleepLockMessage);
        }
        finishSleepPreparation();
        updateControls();
    }

    void finishSleepPreparation()
    {
        if (locking || lockPending) {
            return;
        }
        if (std::exchange(sleepPreparationPending, false)) {
            sleepDelay->release();
        }
        if (loginAvailable && !sleeping && !closing) {
            SleepDelay::acquire(sleepDelay, sleepSignal.connection);
        }
    }

    void lockForDesktopEvent(const char* message)
    {
        if (closing) {
            return;
        }
        if (smoke == SmokeTest::SessionLock) {
            testInterruptedOperation = busy;
        }
        pickerInvalidated = chooser || exportingPickerHandle;
        if (chooser) {
            g_subprocess_send_signal(chooser, SIGTERM);
        }
        if (unlocked || unlocking || lockPending) {
            lockFixture(message);
        } else if (locking) {
            lockedMessage = message;
        } else {
            conceal();
            setStatus(message);
            updateControls();
        }
    }

    void lockFixture(const char* message)
    {
        conceal();
        lockedMessage = message;
        setStatus("Locking fixture…");
        if (busy) {
            lockPending = true;
            updateControls();
        } else {
            completeLock();
        }
    }

    void completeLock()
    {
        locking = true;
        submit([](CoreSession& session) {
                   session.lock();
                   return true;
               }, [this](bool) {
                   locking = false;
                   setStatus(lockedMessage);
                   if (sleepPreparationPending) {
                       finishSleepPreparation();
                   }
               });
    }

    gboolean smokeTick()
    {
        if (++smokeTicks > 800) {
            return finish(1, "timed out");
        }
        if (busy || !ready || exportingPickerHandle) {
            return G_SOURCE_CONTINUE;
        }
        if (smoke == SmokeTest::PickerLifecycle) {
            return pickerLifecycleTick();
        }
        if (smoke == SmokeTest::SessionLock) {
            return sessionLockSmokeTick();
        }
        if (chooser) {
            return G_SOURCE_CONTINUE;
        }
        if (smoke == SmokeTest::Desktop) {
            return desktopSmokeTick();
        }
        switch (smokeStep++) {
        case 0: {
            auto* style = adw_application_get_style_manager(app);
            adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);
            const bool dark = adw_style_manager_get_dark(style);
            adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_LIGHT);
            const bool light = !adw_style_manager_get_dark(style);
            adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
            if (!dark || !light || !GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(GTK_WIDGET(window)))) {
                return finish(1, "Wayland display and light/dark styles");
            }
            gtk_editable_set_text(password, CoreSession::FixturePassword);
            unlock();
            break;
        }
        case 1:
            if (entries.size() != 3) {
                return finish(1, "unlock and list");
            }
            gtk_editable_set_text(title, "GTK 編集済み");
            rename();
            break;
        case 2:
            if (entries.empty() || entries.front().title != "GTK 編集済み") {
                return finish(1, "rename");
            }
            copy();
            break;
        case 3:
            if (!copied || gdk_clipboard_get_content(clipboard) != copied) {
                return finish(1, "clipboard provider");
            }
            if (smoke == SmokeTest::Replacement) {
                auto* replacement = gdk_content_provider_new_typed(G_TYPE_STRING, "other synthetic clipboard value");
                gdk_clipboard_set_content(clipboard, replacement);
                save();
                lock();
                const bool retained = gdk_clipboard_get_content(clipboard) == replacement;
                gdk_clipboard_set_content(clipboard, nullptr);
                g_object_unref(replacement);
                if (!retained || unlocked || !entries.empty()) {
                    return finish(1, "clipboard ownership and concealment during save");
                }
                ++smokeStep;
            }
            break;
        case 4:
            if (copied) {
                --smokeStep;
                break;
            }
            if (!copyExpired || gdk_clipboard_get_content(clipboard)) {
                return finish(1, "clipboard expiry");
            }
            save();
            lock();
            if (unlocked || !entries.empty()) {
                return finish(1, "concealment during save");
            }
            break;
        case 5:
            selectedFile = files.back();
            gtk_editable_set_text(password, CoreSession::FixturePassword);
            unlock();
            break;
        default:
            return entries.size() == 3 && entries.front().title == "GTK 編集済み"
                       ? finish(0, smoke == SmokeTest::Expiry
                                       ? "Wayland, unlock, edit, clipboard expiry, save with pending lock, reopen"
                                       : "Wayland, unlock, edit, clipboard ownership, save with pending lock, reopen")
                       : finish(1, "saved copy reopen");
        }
        return G_SOURCE_CONTINUE;
    }

    gboolean desktopSmokeTick()
    {
        auto* style = adw_application_get_style_manager(app);
        switch (smokeStep++) {
        case 0: {
            struct rlimit limit {};
            if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) != 0 || getrlimit(RLIMIT_CORE, &limit) != 0
                || limit.rlim_max != 0 || !GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(GTK_WIDGET(window)))) {
                return finish(1, "Wayland and dump protection");
            }
            setTestColorScheme("prefer-dark");
            break;
        }
        case 1:
            if (!adw_style_manager_get_dark(style)) {
                return finish(1, "GNOME dark preference");
            }
            setTestColorScheme("prefer-light");
            break;
        case 2:
            if (adw_style_manager_get_dark(style)) {
                return finish(1, "GNOME light preference");
            }
            selectedFile.clear();
            choose();
            break;
        case 3:
            if (selectedFile != files.front()) {
                return finish(1, "file picker result");
            }
            gtk_editable_set_text(password, CoreSession::FixturePassword);
            unlock();
            break;
        default:
            return entries.size() == 3 ? finish(0, "dump protection, live GNOME appearance, portal picker, fixture unlock")
                                       : finish(1, "picked fixture unlock");
        }
        return G_SOURCE_CONTINUE;
    }

    static void setTestColorScheme(const char* preference)
    {
        auto* settings = g_settings_new("org.gnome.desktop.interface");
        g_settings_set_string(settings, "color-scheme", preference);
        g_object_unref(settings);
    }

    gboolean pickerLifecycleTick()
    {
        switch (smokeStep) {
        case 0:
            ++smokeStep;
            choose();
            break;
        case 1:
            if (chooser) {
                break;
            }
            if (selectedFile != files.front() || unlocked || pickerToplevel
                || !g_str_equal(gtk_label_get_text(status), "Fixture selection cancelled.")) {
                return finish(1, "picker cancellation preserves the locked selection");
            }
            ++smokeStep;
            choose();
            break;
        case 2:
            if (!chooser || !pickerToplevel) {
                return finish(1, "second parented picker request");
            }
            if (!pickerCloseAt) {
                pickerCloseAt = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
            } else if (g_get_monotonic_time() >= pickerCloseAt) {
                ++smokeStep;
                gtk_window_close(window);
            }
            break;
        default:
            break;
        }
        return G_SOURCE_CONTINUE;
    }

    bool emitTestLockEvent(const char* source, bool active)
    {
        if (!screenLockSignal.connection) {
            return false;
        }
        auto* reply = g_dbus_connection_call_sync(
            screenLockSignal.connection, "org.example.Phase2.LockSignals", "/org/example/Phase2/LockSignals",
            "org.example.Phase2.LockSignals", "Emit", g_variant_new("(sb)", source, active), G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr);
        if (!reply) {
            return false;
        }
        g_variant_unref(reply);
        return true;
    }

    gboolean sessionLockSmokeTick()
    {
        const auto statusIs = [this](const char* message) {
            return g_str_equal(gtk_label_get_text(status), message);
        };
        const auto emptyAndLocked = [this] {
            return !unlocked && !unlocking && entries.empty() && !copied
                && g_list_model_get_n_items(G_LIST_MODEL(titles)) == 0
                && *gtk_editable_get_text(title) == '\0' && *gtk_editable_get_text(password) == '\0';
        };
        const auto unlockFixture = [this] {
            gtk_editable_set_text(password, CoreSession::FixturePassword);
            unlock();
        };
        switch (smokeStep++) {
        case 0:
            unlockFixture();
            if (!emitTestLockEvent("screen", true)) {
                return finish(1, "screen lock during unlock");
            }
            break;
        case 1:
            if (!emptyAndLocked() || !statusIs(ScreenLockMessage) || !testInterruptedOperation
                || !emitTestLockEvent("screen", false)) {
                return finish(1, "pending unlock was not discarded");
            }
            unlockFixture();
            break;
        case 2:
            if (!unlocked || entries.size() != 3) {
                return finish(1, "explicit unlock after screen lock");
            }
            if (!testClipboardInput) {
                --smokeStep;
                break;
            }
            copy();
            break;
        case 3:
            if (!copied || gdk_clipboard_get_content(clipboard) != copied) {
                return finish(1, "fixture clipboard ownership before screen lock");
            }
            if (!emitTestLockEvent("sleep", false)) {
                return finish(1, "send resume signal");
            }
            break;
        case 4:
            if (!unlocked || !copied || !emitTestLockEvent("screen", true)) {
                return finish(1, "resume signal changed the session");
            }
            break;
        case 5:
            if (!emptyAndLocked() || !statusIs(ScreenLockMessage) || gdk_clipboard_get_content(clipboard)
                || !emitTestLockEvent("screen", false)) {
                return finish(1, "screen lock did not clear the clipboard");
            }
            unlockFixture();
            break;
        case 6:
            if (!unlocked) {
                return finish(1, "unlock before save");
            }
            save();
            if (!emitTestLockEvent("sleep", true)) {
                return finish(1, "pre-suspend signal during save");
            }
            break;
        case 7:
            if (!emptyAndLocked() || !statusIs(SleepLockMessage) || !testInterruptedOperation || files.size() != 2
                || !emitTestLockEvent("sleep", false)) {
                return finish(1, "save completion did not leave the fixture locked");
            }
            break;
        case 8:
            if (!emptyAndLocked() || !statusIs(SleepLockMessage)) {
                return finish(1, "resume unlocked the fixture");
            }
            selectedFile = files.back();
            unlockFixture();
            break;
        case 9:
            if (!unlocked || entries.size() != 3) {
                return finish(1, "saved copy did not reopen");
            }
            lock();
            break;
        case 10:
            choose();
            break;
        case 11:
            if (!chooser) {
                return finish(1, "open picker before screen lock");
            }
            if (!pickerCloseAt) {
                pickerCloseAt = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
            }
            if (g_get_monotonic_time() < pickerCloseAt) {
                --smokeStep;
            } else if (!emitTestLockEvent("screen", true)) {
                return finish(1, "screen lock while picker was open");
            }
            break;
        default:
            if (chooser) {
                --smokeStep;
                break;
            }
            return emptyAndLocked() && statusIs(ScreenLockMessage) && selectedFile == files.back()
                       ? finish(0, "screen lock, pre-suspend lock, clipboard clearing, interrupted unlock/save, picker close")
                       : finish(1, "desktop lock while picker was open");
        }
        return G_SOURCE_CONTINUE;
    }

    gboolean finish(int statusCode, const char* message)
    {
        std::fprintf(statusCode == 0 ? stdout : stderr, "%s: %s\n", statusCode == 0 ? "PASS" : "FAIL", message);
        exitStatus = statusCode;
        closing = true;
        stopWhenIdle();
        return G_SOURCE_REMOVE;
    }

    AdwApplication* app;
    SmokeTest smoke;
    QTemporaryDir directory;
    // Keep the delay descriptor alive until the worker has dropped any open database.
    std::shared_ptr<SleepDelay> sleepDelay = std::make_shared<SleepDelay>();
    CoreWorker worker;
    GtkWindow* window = nullptr;
    GtkLabel* status = nullptr;
    GtkEditable* password = nullptr;
    GtkEditable* title = nullptr;
    GtkDropDown* selection = nullptr;
    GtkStringList* titles = nullptr;
    GtkWidget* chooseButton = nullptr;
    GtkWidget* unlockButton = nullptr;
    GtkWidget* renameButton = nullptr;
    GtkWidget* saveButton = nullptr;
    GtkWidget* copyButton = nullptr;
    GtkWidget* lockButton = nullptr;
    GSubprocess* chooser = nullptr;
    GdkToplevel* pickerToplevel = nullptr;
    GSettings* appearanceSettings = nullptr;
    GdkClipboard* clipboard = nullptr;
    GdkContentProvider* copied = nullptr;
    LockSignal screenLockSignal;
    LockSignal sleepSignal;
    guint loginWatch = 0;
    guint copyTimer = 0;
    guint smokeTimer = 0;
    unsigned nextCopyNumber = 1;
    int smokeStep = 0;
    int smokeTicks = 0;
    gint64 pickerCloseAt = 0;
    bool ready = false;
    bool busy = false;
    bool unlocked = false;
    bool unlocking = false;
    bool lockPending = false;
    bool locking = false;
    bool sleeping = false;
    bool sleepPreparationPending = false;
    bool loginAvailable = false;
    bool closing = false;
    bool copyExpired = false;
    bool exportingPickerHandle = false;
    bool pickerInvalidated = false;
    bool testInterruptedOperation = false;
    bool testClipboardInput = false;
    const char* lockedMessage = "Locked.";
    std::string pickerHandle;
    std::string selectedFile;
    std::vector<std::string> files;
    std::vector<EntrySummary> entries;
};
}

int main(int argc, char** argv)
{
    auto smoke = SmokeTest::None;
    if (argc == 2 && std::strcmp(argv[1], "--smoke-test") == 0) {
        smoke = SmokeTest::Expiry;
    } else if (argc == 2 && std::strcmp(argv[1], "--smoke-test-replacement") == 0) {
        smoke = SmokeTest::Replacement;
    } else if (argc == 2 && std::strcmp(argv[1], "--smoke-test-desktop") == 0
               && g_strcmp0(g_getenv("GSETTINGS_BACKEND"), "memory") == 0) {
        smoke = SmokeTest::Desktop;
    } else if (argc == 2 && std::strcmp(argv[1], "--smoke-test-picker-lifecycle") == 0) {
        smoke = SmokeTest::PickerLifecycle;
    } else if (argc == 2 && std::strcmp(argv[1], "--smoke-test-session-lock") == 0) {
        smoke = SmokeTest::SessionLock;
    } else if (argc != 1) {
        std::fputs("Usage: phase2_gtk_probe [--smoke-test | --smoke-test-replacement | --smoke-test-desktop | "
                   "--smoke-test-picker-lifecycle | --smoke-test-session-lock]\n", stderr);
        return 1;
    }
    QCoreApplication qt(argc, argv);
    if (!CoreSession::initialize()) {
        std::fputs("Upstream crypto self-test failed\n", stderr);
        return 1;
    }
    auto* app = adw_application_new("org.example.KeePassXcGnome.Phase2", G_APPLICATION_NON_UNIQUE);
    int result;
    {
        Window window(app, smoke);
        g_signal_connect(app, "activate", G_CALLBACK(+[](GApplication*, gpointer data) {
            static_cast<Window*>(data)->activate();
        }), &window);
        result = g_application_run(G_APPLICATION(app), 1, argv);
        if (window.exitStatus) {
            result = window.exitStatus;
        }
    }
    g_object_unref(app);
    return result;
}
