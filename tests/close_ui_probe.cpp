// Run the real close/save UI against synthetic encrypted databases and a
// separate private chooser bus. No test controls enter the product binary.
#define main keepassxc_gnome_product_main
#include "../src/main.cpp"
#undef main
#include "PickerPortal.h"
#include <QTemporaryDir>
#include <QFile>
#include <cstring>

namespace {
struct CloseUiProbe {
    AdwApplication* app;
    std::unique_ptr<Window> ui;
    std::unique_ptr<PickerTest::Portal> portal;
    QTemporaryDir files;
    std::string source, lateSource, target, pickerExecutable;
    QByteArray original, external;
    unsigned stage = 0, ticks = 0, checks = 0;
    gint64 dialogShown = 0;
    unsigned lastLoggedStage = ~0U;
    explicit CloseUiProbe(AdwApplication* app) : app(app), files(QString::fromLocal8Bit(g_getenv("KPXC_UI_ROOT")) + "/close.XXXXXX") {
        check(files.isValid(), "private test folder");
        source = files.filePath("source.kdbx").toStdString(); lateSource = files.filePath("late.kdbx").toStdString(); target = files.filePath("new database.kdbx").toStdString();
        check(CoreSession::createFixture(source, FixtureFormat::Kdbx3) && CoreSession::createFixture(lateSource, FixtureFormat::Kdbx3), "upstream encrypted fixtures"); original = load(source);
    }
    void check(bool ok, const char* label) { if (!ok) { std::fprintf(stderr, "FAIL close UI: %s\n", label); std::exit(2); } ++checks; }
    QByteArray load(const std::string& path) { QFile f(QString::fromStdString(path)); check(f.open(QIODevice::ReadOnly), "read synthetic file"); return f.readAll(); }
    static GtkWidget* find(GtkWidget* root, GType type, const char* label = nullptr) {
        if (G_TYPE_CHECK_INSTANCE_TYPE(root, type)) {
            const char* text = GTK_IS_BUTTON(root) ? gtk_button_get_label(GTK_BUTTON(root)) : nullptr;
            if (!label || (text && !std::strcmp(text, label))) return root;
        }
        for (auto* child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) if (auto* result = find(child, type, label)) return result;
        return nullptr;
    }
    AdwDialog* dialog() { return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window)); }
    void click(const char* text) {
        check(dialog(), "visible dialog"); auto* button = find(GTK_WIDGET(dialog()), GTK_TYPE_BUTTON, tr(text));
        check(button && gtk_widget_get_sensitive(button), "enabled dialog response"); g_signal_emit_by_name(button, "clicked");
    }
    void unlock() {
        check(dialog() && ADW_IS_ALERT_DIALOG(dialog()), "ordinary unlock dialog");
        auto* field = find(GTK_WIDGET(dialog()), GTK_TYPE_PASSWORD_ENTRY); check(field, "password field");
        gtk_editable_set_text(GTK_EDITABLE(field), CoreSession::FixturePassword); click("Unlock");
    }
    void restart() {
        check(ui->closing, "previous window closed only after chosen outcome"); ui.reset();
        ui = std::make_unique<Window>(app, pickerExecutable); ui->present();
    }
    void rename(const char* title) {
        ui->submit([title](RemoteSession& s) { return s.rename(s.entries().front().id, title); }, [this](bool ok) {
            check(ok, "confirmed synthetic edit"); ui->dirty = true; ui->refresh();
        });
    }
    void create() {
        const auto root = ui->checkpointFolder;
        ui->submit([root](RemoteSession& s) { return s.create("Close test", CoreSession::FixturePassword) && s.enableRecovery(root); }, [this](bool ok) {
            check(ok, "new unsaved model protected"); ui->hasOpenedDatabase = ui->unlocked = ui->dirty = true;
            ui->sourceKnown = false; gtk_stack_set_visible_child_name(ui->stack, "unlocked"); ui->refresh();
        });
    }
    void closeDialog(bool associated) {
        check(dialog() && ADW_IS_ALERT_DIALOG(dialog()), "close confirmation shown"); auto* d = ADW_ALERT_DIALOG(dialog());
        check(!std::strcmp(adw_alert_dialog_get_heading(d), tr("Save changes before closing?")), "localized close heading");
        check(adw_alert_dialog_has_response(d, "cancel") && adw_alert_dialog_has_response(d, "discard") && adw_alert_dialog_has_response(d, "save"), "cancel/discard/save responses");
        check(!std::strcmp(adw_alert_dialog_get_response_label(d, "save"), associated ? tr("Save (overwrite)") : tr("Save as…")), "source-aware save label");
        check(!std::strcmp(adw_alert_dialog_get_close_response(d), "cancel") && !std::strcmp(adw_alert_dialog_get_default_response(d), "save"), "escape cancels, default saves");
    }
    void snapshot() {
        auto* widget = GTK_WIDGET(ui->window); auto* paintable = gtk_widget_paintable_new(widget); auto* shot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(shot), gtk_widget_get_width(widget), gtk_widget_get_height(widget));
        auto* node = gtk_snapshot_free_to_node(shot); check(node, "rendered close confirmation");
        auto* texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(widget)), node, nullptr);
        auto* path = g_build_filename(g_getenv("KPXC_UI_ROOT"), "save-before-close.png", nullptr);
        check(gdk_texture_save_to_png(texture, path), "close confirmation screenshot");
        g_free(path); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
    }
    gboolean tick() {
        check(++ticks < 1200, "bounded close workflow"); auto& w = *ui;
        if (w.busy || (!w.closing && w.guard && !w.guard->settled())) return G_SOURCE_CONTINUE;
        if (lastLoggedStage != stage) { std::fprintf(stderr, "close stage=%u\n", stage); lastLoggedStage = stage; }
        switch (stage) {
        case 0: w.credentials(false, source); break;
        case 1: unlock(); break;
        case 2:
            check(w.unlocked && !w.dirty && !gtk_widget_get_visible(w.dirtyIndicator), "clean file has no modified indicator"); rename("before-close"); break;
        case 3:
            check(w.dirty && gtk_widget_get_visible(w.dirtyIndicator) && gtk_widget_has_css_class(w.saveButton, "suggested-action"), "confirmed unsaved edits have text/icon and prominent Save");
            check(g_str_has_prefix(adw_window_title_get_title(w.windowTitle), "• "), "title carries a modified mark"); gtk_window_close(w.window); dialogShown = g_get_monotonic_time(); break;
        case 4:
            if (g_get_monotonic_time() - dialogShown < 350000) return G_SOURCE_CONTINUE;
            closeDialog(true); snapshot(); click("Cancel"); break;
        case 5:
            check(!w.closing && w.dirty && load(source) == original, "cancel preserves dirty model and original"); gtk_window_close(w.window); break;
        case 6:
            closeDialog(true); click("Save (overwrite)"); gtk_window_close(w.window);
            check(w.closeSavePending && !w.closing, "repeated close waits for durable save"); break;
        case 7: {
            check(w.closing && load(source) != original, "successful overwrite closes after commit");
            CoreSession verify; check(verify.unlock(source, CoreSession::FixturePassword) && verify.entries().front().title == "before-close", "closed file contains confirmed changes");
            restart(); break;
        }
        case 8: create(); break;
        case 9: gtk_window_close(w.window); break;
        case 10: closeDialog(false); portal->reply = PickerTest::Portal::Cancel; click("Save as…"); break;
        case 11:
            if (w.picker.active() || w.closeSavePending) return G_SOURCE_CONTINUE;
            check(!w.closing && w.unlocked && w.dirty && !std::filesystem::exists(target), "cancelled first-save chooser leaves model open"); gtk_window_close(w.window); break;
        case 12: closeDialog(false); portal->reply = PickerTest::Portal::Fail; click("Save as…"); break;
        case 13:
            if (w.picker.active() || w.closeSavePending) return G_SOURCE_CONTINUE;
            check(!w.closing && w.dirty, "failed chooser leaves model open"); gtk_window_close(w.window); break;
        case 14: closeDialog(false); portal->reply = PickerTest::Portal::Hold; click("Save as…"); break;
        case 15:
            if (portal->calls < 3) return G_SOURCE_CONTINUE;
            check(w.closeSavePending && w.picker.active(), "first save awaits native chooser"); w.automaticLock(); break;
        case 16:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            check(!w.closing && !w.unlocked && !w.closeSavePending && !std::filesystem::exists(target), "lock cancels pending save/close, never closes after lock"); create(); break;
        case 17: gtk_window_close(w.window); break;
        case 18: closeDialog(false); portal->reply = PickerTest::Portal::Select; click("Save as…"); break;
        case 19: {
            if (w.picker.active() || w.closeSavePending) return G_SOURCE_CONTINUE;
            check(w.closing && std::filesystem::exists(target), "first save closes only after durable publication");
            CoreSession verify; check(verify.unlock(target, CoreSession::FixturePassword) && verify.groups().front().name == "Close test", "new chosen file can be reopened");
            restart(); break;
        }
        case 20: w.credentials(false, source); break;
        case 21: unlock(); break;
        case 22: rename("conflict-edit"); break;
        case 23: {
            external = load(source) + QByteArray("synthetic-external-update");
            QFile f(QString::fromStdString(source)); check(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(external) == external.size(), "external change fixture"); f.close();
            gtk_window_close(w.window); break;
        }
        case 24: closeDialog(true); click("Save (overwrite)"); break;
        case 25:
            std::fprintf(stderr, "conflict state: closing=%d unlocked=%d dirty=%d pendingClose=%d status=%s\n", w.closing, w.unlocked, w.dirty, w.closeSavePending, gtk_label_get_text(w.status));
            check(!w.closing && w.unlocked && w.dirty && !w.closeSavePending && load(source) == external, "conflict never closes or overwrites external file");
            check(gtk_widget_get_visible(w.dirtyIndicator) && !std::strcmp(gtk_label_get_text(w.status), tr("The destination changed outside this application. Review recovery files and save your changes under a new name.")), "failure preserves modified indicator and reason");
            w.edit({"", w.selectedGroup, "unconfirmed form", "synthetic-user", "synthetic-password", "", ""}); w.requestClose(); break;
        case 26:
            check(dialog() && !std::strcmp(adw_dialog_get_title(dialog()), tr("New entry")) && !w.closing, "main close does not silently save/discard unconfirmed form input");
            adw_dialog_close(dialog()); break;
        case 27: gtk_window_close(w.window); break;
        case 28: closeDialog(true); click("Discard"); break;
        case 29:
            check(w.closing && load(source) == external, "explicit discard closes without writing"); restart(); break;
        case 30: w.credentials(false, lateSource); break;
        case 31: unlock(); break;
        case 32: rename("late-save"); break;
        case 33: gtk_window_close(w.window); break;
        case 34:
            click("Save (overwrite)"); check(w.closeSavePending, "save completion is pending"); w.automaticLock(); break;
        case 35: {
            check(!w.unlocked && !w.closing && !w.closeSavePending && !gtk_widget_get_visible(w.dirtyIndicator), "lock cancels close continuation and conceals modified state");
            CoreSession verify; check(verify.unlock(lateSource, CoreSession::FixturePassword), "interrupted save leaves a valid original or committed file");
            gtk_window_close(w.window); check(w.closing, "locked window can close");
            std::printf("PASS close GUI: %u checks, cancel, overwrite, first-save cancel/failure/success, conflict, explicit discard, active form, repeated close and lock invalidation\n", checks); std::fflush(stdout);
            g_application_release(G_APPLICATION(app)); return G_SOURCE_REMOVE;
        }
        }
        ++stage; return G_SOURCE_CONTINUE;
    }
};
}
int main(int argc, char** argv) {
    initializeUiLocale(); const auto locale = localeDirectory();
    bindtextdomain("keepassxc-gnome", locale.c_str()); bind_textdomain_codeset("keepassxc-gnome", "UTF-8"); textdomain("keepassxc-gnome");
    auto* launcher = g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    auto parent = getpid();
    g_subprocess_launcher_set_child_setup(launcher, +[](gpointer p) {
        const auto parent = *static_cast<pid_t*>(p); if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(2);
    }, &parent, nullptr);
    auto* pickerBus = g_subprocess_launcher_spawn(launcher, nullptr, "dbus-daemon", "--session", "--nofork", "--print-address=1", nullptr); g_object_unref(launcher);
    PickerTest::check(pickerBus, "private chooser bus"); auto* input = g_data_input_stream_new(g_subprocess_get_stdout_pipe(pickerBus));
    auto* address = g_data_input_stream_read_line(input, nullptr, nullptr, nullptr); PickerTest::check(address, "chooser bus address");
    const std::string bus(address); g_free(address); g_object_unref(input);
    QCoreApplication qt(argc, argv); if (!CoreSession::initialize()) return 1;
    auto* app = adw_application_new("org.keepassxc.Gnome.CloseTest", G_APPLICATION_DEFAULT_FLAGS);
    CloseUiProbe probe(app); probe.portal = std::make_unique<PickerTest::Portal>(bus.c_str());
    probe.portal->wayland = probe.portal->save = true; probe.portal->expectedTitle = tr("Save as a new file"); probe.portal->expectedName = "Passwords.kdbx";
    auto* uri = g_filename_to_uri(probe.target.c_str(), nullptr, nullptr); PickerTest::check(uri, "synthetic target URI"); probe.portal->selectedUri = uri; g_free(uri);
    probe.pickerExecutable = std::string(g_getenv("KPXC_UI_ROOT")) + "/picker-wrapper";
    const auto helper = std::filesystem::canonical("/proc/self/exe").parent_path() / "keepassxc-gnome-picker";
    auto* quotedBus = g_shell_quote(bus.c_str()); auto* quotedHelper = g_shell_quote(helper.c_str());
    const auto script = std::string("#!/bin/sh\nDBUS_SESSION_BUS_ADDRESS=") + quotedBus + " exec " + quotedHelper + " \"$@\"\n";
    PickerTest::check(g_file_set_contents(probe.pickerExecutable.c_str(), script.c_str(), script.size(), nullptr) && !g_chmod(probe.pickerExecutable.c_str(), 0700), "isolated chooser wrapper"); g_free(quotedBus); g_free(quotedHelper);
    g_signal_connect(app, "activate", G_CALLBACK(+[](AdwApplication* app, gpointer data) {
        auto& p = *static_cast<CloseUiProbe*>(data); g_application_hold(G_APPLICATION(app)); p.ui = std::make_unique<Window>(app, p.pickerExecutable); p.ui->present();
        g_timeout_add(70, +[](gpointer data) { return static_cast<CloseUiProbe*>(data)->tick(); }, &p);
    }), &probe);
    const auto result = g_application_run(G_APPLICATION(app), 1, argv); probe.ui.reset(); probe.portal.reset(); g_object_unref(app);
    g_unlink(probe.pickerExecutable.c_str()); g_subprocess_force_exit(pickerBus); g_subprocess_wait(pickerBus, nullptr, nullptr); g_object_unref(pickerBus); return result;
}
