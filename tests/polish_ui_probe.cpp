// Runs the product Window in a disposable Wayland session. All displayed
// accounts below are synthetic; stress rows do not enter the database model.
#define main keepassxc_gnome_product_main
#include "../src/main.cpp"
#undef main
#include <cstring>
#include "PickerPortal.h"

namespace {
struct PolishUiProbe {
    std::unique_ptr<PickerTest::Portal> portal;
    std::unique_ptr<Window> ui;
    bool cancelled = false, selected = false;
    std::string pickerExecutable;
    GSettings* settings = nullptr;
    unsigned stage = 0, ticks = 0, checks = 0;
    void check(bool ok, const char* label) { if (!ok) { std::fprintf(stderr, "FAIL polish UI: %s\n", label); std::exit(2); } ++checks; }
    unsigned rows(GtkWidget* widget) {
        unsigned count = ADW_IS_ACTION_ROW(widget) ? 1 : 0;
        for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) count += rows(child);
        return count;
    }
    void snapshot(const char* name) {
        auto* widget = GTK_WIDGET(ui->window); auto* paintable = gtk_widget_paintable_new(widget);
        auto* snapshot = gtk_snapshot_new(); const auto width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
        auto* node = gtk_snapshot_free_to_node(snapshot); check(node, "window render node");
        auto* texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(widget)), node, nullptr);
        auto* path = g_build_filename(g_getenv("KPXC_UI_ROOT"), name, nullptr);
        check(gdk_texture_save_to_png(texture, path), "actual GTK screenshot");
        g_free(path); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
    }
    gboolean tick() {
        check(++ticks < 600, "UI test bounded"); auto& w = *ui;
        if (w.busy) return G_SOURCE_CONTINUE;
        switch (stage) {
        case 0: {
            if (!gtk_widget_get_mapped(GTK_WIDGET(w.window)) || (w.guard && !w.guard->settled())) return G_SOURCE_CONTINUE;
            settings = g_settings_new("org.gnome.desktop.interface");
            check(g_settings_set_string(settings, "color-scheme", "prefer-light"), "private memory-only light setting");
            w.refreshing = true; w.unlocked = true; w.dirty = true; w.selectedGroup = "g0";
            std::vector<std::string> ids;
            for (unsigned i = 0; i < 1200; ++i) {
                const auto id = "g" + std::to_string(i);
                w.groupRows.push_back({id, i == 0 ? "GNOME Demo" : i == 1 ? "仕事 / Work" : "合成グループ / Synthetic " + std::to_string(i), i == 0 ? 0U : 1U}); ids.push_back(id);
            }
            w.groups->append(ids); w.groups->select(0); ids.clear();
            for (unsigned i = 0; i < 20000; ++i) {
                const auto id = "e" + std::to_string(i);
                w.entryRows.push_back({id, i == 0 ? "メール / Mail" : i == 1 ? "クラウド / Cloud" : "合成エントリー / Synthetic " + std::to_string(i), "demo" + std::to_string(i) + "@example.invalid", "g0"}); ids.push_back(id);
            }
            w.entries->append(ids); w.entries->select(0); w.refreshing = false;
            gtk_widget_set_visible(w.emptyLabel, FALSE); gtk_stack_set_visible_child_name(w.stack, "unlocked"); w.sensitivity(); w.message(tr("Unsaved changes"));
            break;
        }
        case 1:
            check(!adw_style_manager_get_dark(adw_application_get_style_manager(w.application)), "system light preference applied");
            check(rows(w.entries->widget()) > 0 && rows(w.entries->widget()) < 1000, "twenty thousand entries create a bounded set of reusable rows");
            check(!adw_overlay_split_view_get_collapsed(w.split) && adw_overlay_split_view_get_show_sidebar(w.split), "wide window shows sidebar");
            snapshot("wide-light.png");
            check(g_settings_set_string(settings, "color-scheme", "prefer-dark"), "private memory-only dark setting"); break;
        case 2:
            check(adw_style_manager_get_dark(adw_application_get_style_manager(w.application)), "system dark preference applied dynamically"); snapshot("wide-dark.png");
            gtk_window_set_default_size(w.window, 560, 620); break;
        case 3:
            check(adw_overlay_split_view_get_collapsed(w.split) && gtk_widget_get_visible(w.sidebarButton), "narrow window exposes collapsible groups");
            check(gtk_widget_get_width(GTK_WIDGET(w.window)) == 560, "minimum window width respected");
            snapshot("narrow-dark.png"); adw_overlay_split_view_set_show_sidebar(w.split, TRUE); break;
        case 4:
            check(adw_overlay_split_view_get_show_sidebar(w.split), "group sidebar opens in narrow window"); snapshot("narrow-groups.png");
            adw_overlay_split_view_set_show_sidebar(w.split, FALSE);
            w.edit({"e0", "g0", "メール / Mail", "demo@example.invalid", "synthetic-screenshot-password", "https://example.invalid", "Synthetic notes only"}); break;
        case 5:
            check(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w.window)), "editor opens at narrow width"); snapshot("narrow-editor.png");
            w.automaticLock(); break;
        case 6:
            check(!w.unlocked && w.entryRows.empty() && w.groupRows.empty() && rows(w.entries->widget()) == 0, "lock clears virtualized models and account text");
            check(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w.window)), "lock closes editor");
            snapshot("locked-dark.png");
            portal->reply = PickerTest::Portal::Hold;
            check(w.picker.choose(w.window, {NativePicker::Mode::Open, "合成データを選択", {}, {}}, [this](auto status, auto path) {
                check(status == NativePicker::Status::Cancelled && path.empty(), "lock discards native picker result"); cancelled = true;
            }), "Wayland parent picker started");
            break;
        case 7:
            if (!portal->calls) return G_SOURCE_CONTINUE;
            w.automaticLock(); break;
        case 8:
            if (!cancelled) return G_SOURCE_CONTINUE;
            check(!w.picker.active() && portal->closes == 1 && portal->parentHandles == 1, "lock closes and reaps parented native request");
            portal->reply = PickerTest::Portal::Select;
            check(w.picker.choose(w.window, {NativePicker::Mode::Open, "合成データを選択", {}, {}}, [this](auto status, auto path) {
                check(status == NativePicker::Status::Selected && path == "/tmp/合成 test.kdbx", "Wayland picker can select after lock cancellation"); selected = true;
            }), "Wayland parent handle can be reused"); break;
        case 9:
            if (!selected) return G_SOURCE_CONTINUE;
            check(portal->parentHandles == 2, "each picker exports its own Wayland parent handle");
            w.closing = true; w.worker.shutdown(); gtk_window_destroy(w.window); g_clear_object(&settings);
            std::printf("PASS polish GUI: %u checks, virtualized lists, responsive layouts, system theme changes, Wayland picker lifecycle and screenshots\n", checks); std::fflush(stdout);
            return G_SOURCE_REMOVE;
        }
        ++stage; return G_SOURCE_CONTINUE;
    }
};
}
int main(int argc, char** argv) {
    initializeUiLocale();
    const auto locale = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "share/locale";
    bindtextdomain("keepassxc-gnome", locale.c_str()); bind_textdomain_codeset("keepassxc-gnome", "UTF-8"); textdomain("keepassxc-gnome");
    // A second disposable bus for the metadata helper only. Using GTestDBus
    // here would also wait for GTK's unrelated singleton desktop connection.
    auto* launcher = g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    auto parent = getpid();
    g_subprocess_launcher_set_child_setup(launcher, +[](gpointer data) {
        const auto parent = *static_cast<pid_t*>(data);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(2);
    }, &parent, nullptr);
    auto* pickerBus = g_subprocess_launcher_spawn(launcher, nullptr, "dbus-daemon", "--session", "--nofork", "--print-address=1", nullptr);
    g_object_unref(launcher); PickerTest::check(pickerBus, "private chooser bus process");
    auto* input = g_data_input_stream_new(g_subprocess_get_stdout_pipe(pickerBus));
    auto* address = g_data_input_stream_read_line(input, nullptr, nullptr, nullptr);
    PickerTest::check(address && std::strlen(address) < 4096, "private chooser bus address");
    const std::string pickerAddress(address); g_free(address); g_object_unref(input);
    QCoreApplication qt(argc, argv); if (!CoreSession::initialize()) return 1;
    PolishUiProbe probe;
    probe.portal = std::make_unique<PickerTest::Portal>(pickerAddress.c_str()); probe.portal->wayland = true;
    probe.pickerExecutable = std::string(g_getenv("KPXC_UI_ROOT")) + "/picker-wrapper";
    const auto helper = std::filesystem::canonical("/proc/self/exe").parent_path() / "keepassxc-gnome-picker";
    auto* quotedBus = g_shell_quote(pickerAddress.c_str()); auto* quotedHelper = g_shell_quote(helper.c_str());
    const auto script = std::string("#!/bin/sh\nDBUS_SESSION_BUS_ADDRESS=") + quotedBus + " exec " + quotedHelper + " \"$@\"\n";
    probe.check(g_file_set_contents(probe.pickerExecutable.c_str(), script.c_str(), script.size(), nullptr) && !g_chmod(probe.pickerExecutable.c_str(), 0700), "private metadata-helper launcher");
    g_free(quotedBus); g_free(quotedHelper);
    auto* app = adw_application_new("org.keepassxc.Gnome.PolishTest", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(+[](AdwApplication* app, gpointer data) {
        auto& probe = *static_cast<PolishUiProbe*>(data); probe.ui = std::make_unique<Window>(app, probe.pickerExecutable); probe.ui->present();
        g_timeout_add(450, +[](gpointer data) { return static_cast<PolishUiProbe*>(data)->tick(); }, &probe);
    }), &probe);
    const auto result = g_application_run(G_APPLICATION(app), 1, argv); probe.ui.reset(); g_object_unref(app); probe.portal.reset(); g_unlink(probe.pickerExecutable.c_str());
    g_subprocess_force_exit(pickerBus); g_subprocess_wait(pickerBus, nullptr, nullptr); g_object_unref(pickerBus); return result;
}
