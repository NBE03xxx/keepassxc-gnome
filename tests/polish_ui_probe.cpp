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
    unsigned stage = 0, ticks = 0, checks = 0, colorIndex = 0;
    static constexpr const char* colors[] = {"blue", "teal", "green", "yellow", "orange", "red", "pink", "purple", "slate", "brown"};
    void check(bool ok, const char* label) { if (!ok) { std::fprintf(stderr, "FAIL polish UI: %s\n", label); std::exit(2); } ++checks; }
    unsigned rows(GtkWidget* widget) {
        unsigned count = g_object_get_data(G_OBJECT(widget), "summary-title") ? 1 : 0;
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
    GtkWidget* firstRow(GtkWidget* widget) {
        if (g_object_get_data(G_OBJECT(widget), "summary-title")) return widget;
        for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
            if (auto* row = firstRow(child)) return row;
        return nullptr;
    }
    void accentPixel() {
        std::fprintf(stderr, "accent render: stage=%u color=%s\n", stage, colors[colorIndex]);
        auto* row = firstRow(ui->entries->widget()); check(row, "selected entry row bound");
        while (row && std::strcmp(gtk_widget_get_css_name(row), "row")) row = gtk_widget_get_parent(row);
        check(row && (gtk_widget_get_state_flags(row) & GTK_STATE_FLAG_SELECTED), "selected entry CSS row");
        graphene_rect_t bounds; check(gtk_widget_compute_bounds(row, GTK_WIDGET(ui->window), &bounds), "selected row bounds");
        auto* paintable = gtk_widget_paintable_new(GTK_WIDGET(ui->window)); auto* shot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(shot), gtk_widget_get_width(GTK_WIDGET(ui->window)), gtk_widget_get_height(GTK_WIDGET(ui->window)));
        auto* node = gtk_snapshot_free_to_node(shot); check(node, "accent render node");
        auto* texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(ui->window)), node, nullptr);
        auto* downloader = gdk_texture_downloader_new(texture); gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
        gdk_texture_downloader_set_color_state(downloader, gdk_color_state_get_srgb());
        gsize stride = 0; auto* bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
        const auto scale = double(gdk_texture_get_width(texture)) / gtk_widget_get_width(GTK_WIDGET(ui->window));
        const auto x = size_t((bounds.origin.x + bounds.size.width - 16) * scale), y = size_t((bounds.origin.y + bounds.size.height / 2) * scale);
        const auto* data = static_cast<const unsigned char*>(g_bytes_get_data(bytes, nullptr));
        auto* cls = G_ENUM_CLASS(g_type_class_ref(ADW_TYPE_ACCENT_COLOR)); const auto* value = g_enum_get_value_by_nick(cls, colors[colorIndex]);
        GdkRGBA expected; adw_accent_color_to_rgba(value ? static_cast<AdwAccentColor>(value->value) : ADW_ACCENT_COLOR_BLUE, &expected); g_type_class_unref(cls);
        auto* plain = gtk_widget_get_next_sibling(row);
        while (plain && std::strcmp(gtk_widget_get_css_name(plain), "row")) plain = gtk_widget_get_next_sibling(plain);
        check(plain && !(gtk_widget_get_state_flags(plain) & GTK_STATE_FLAG_SELECTED), "unselected row for actual view background");
        // The boxed-list card can differ from --view-bg-color in dark mode.
        // This compatibility alias is only a test oracle for the unchanged
        // toolkit view background; the product uses modern CSS variables.
        GdkRGBA background;
        check(gtk_style_context_lookup_color(gtk_widget_get_style_context(row), "view_bg_color", &background), "toolkit view background color");
        const auto* pixel = data + y * stride + x * 4;
        const bool ok = std::abs(int(pixel[0]) - int(expected.red * 255 * .2 + background.red * 255 * .8 + .5)) <= 3 &&
            std::abs(int(pixel[1]) - int(expected.green * 255 * .2 + background.green * 255 * .8 + .5)) <= 3 &&
            std::abs(int(pixel[2]) - int(expected.blue * 255 * .2 + background.blue * 255 * .8 + .5)) <= 3;
        if (!ok) std::fprintf(stderr, "tint %s: actual %u %u %u\n", colors[colorIndex],pixel[0],pixel[1],pixel[2]);
        check(ok, "actual selected-row pixels use a fixed twenty-percent accent mix");
        graphene_rect_t button;
        check(gtk_widget_compute_bounds(ui->saveButton, GTK_WIDGET(ui->window), &button), "Save button bounds");
        const auto bx = size_t((button.origin.x + button.size.width - 8) * scale), by = size_t((button.origin.y + button.size.height / 2) * scale);
        const auto* buttonPixel = data + by * stride + bx * 4;
        const bool buttonOk = std::abs(int(buttonPixel[0]) - int(expected.red * 255 + .5)) <= 3 && std::abs(int(buttonPixel[1]) - int(expected.green * 255 + .5)) <= 3 && std::abs(int(buttonPixel[2]) - int(expected.blue * 255 + .5)) <= 3;
        if (!buttonOk) std::fprintf(stderr, "Save accent %s: actual %u %u %u\n", colors[colorIndex],buttonPixel[0],buttonPixel[1],buttonPixel[2]);
        check(buttonOk, "actual primary Save button follows desktop accent preference");
        GdkRGBA foreground; gtk_widget_get_color(row, &foreground);
        GdkRGBA normalText; gtk_widget_get_color(plain, &normalText);
        check(std::abs(foreground.red-normalText.red)<.01 && std::abs(foreground.green-normalText.green)<.01 &&
            std::abs(foreground.blue-normalText.blue)<.01 && std::abs(foreground.alpha-normalText.alpha)<.01 && gtk_widget_get_opacity(row)>.99, "selected text and widget retain their standard opacity");
        g_bytes_unref(bytes); gdk_texture_downloader_free(downloader); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
    }
    gboolean tick() {
        check(++ticks < 600, "UI test bounded"); auto& w = *ui;
        if (w.busy) return G_SOURCE_CONTINUE;
        switch (stage) {
        case 0: {
            if (!gtk_widget_get_mapped(GTK_WIDGET(w.window)) || (w.guard && !w.guard->settled())) return G_SOURCE_CONTINUE;
            check(!std::strcmp(adw_status_page_get_title(ADW_STATUS_PAGE(w.welcome)), tr("Welcome to KeePassXC GNOME")), "startup has no open database rather than a fictitious lock");
            settings = g_settings_new("org.gnome.desktop.interface");
            check(g_settings_set_string(settings, "color-scheme", "prefer-light"), "private memory-only light setting");
            check(g_settings_set_string(settings, "accent-color", colors[0]), "private initial accent setting");
            check(!adw_style_manager_get_system_supports_accent_colors(adw_application_get_style_manager(w.application)), "private portal exercises read-only desktop accent fallback");
            w.refreshing = true; w.hasOpenedDatabase = w.unlocked = true; w.dirty = true; w.selectedGroup = "g0";
            std::vector<std::string> ids;
            for (unsigned i = 0; i < 1200; ++i) {
                const auto id = "g" + std::to_string(i);
                w.groupRows.push_back({id, i == 0 ? "GNOME Demo" : i == 1 ? "仕事 / Work" : "合成グループ / Synthetic " + std::to_string(i), i == 0 ? 0U : 1U}); ids.push_back(id);
            }
            w.groups->reset(w.groupRows, {}, false); w.groups->select({true, 0}); ids.clear();
            check(w.groups->visibleCount() == 1 && w.groups->expanded().empty(), "group tree starts collapsed");
            for (unsigned i = 0; i < 20000; ++i) {
                const auto id = "e" + std::to_string(i);
                w.entryRows.push_back({id, i == 0 ? "メール / Mail" : i == 1 ? "クラウド / Cloud" : "合成エントリー / Synthetic " + std::to_string(i), "demo" + std::to_string(i) + "@example.invalid", "g0"}); ids.push_back(id);
            }
            w.entries->append(ids); w.entries->select(0); w.refreshing = false;
            gtk_widget_set_visible(w.emptyLabel, FALSE); gtk_stack_set_visible_child_name(w.stack, "unlocked"); w.sensitivity(); w.message(tr("Unsaved changes"));
            gtk_label_set_text(w.previewTitle, "メール / Mail"); gtk_label_set_text(w.previewUsername, "demo@example.invalid");
            gtk_editable_set_text(w.previewPassword, "synthetic-preview-password");
            gtk_label_set_text(w.previewUrl, "https://example.invalid"); gtk_label_set_text(w.previewNotes, "合成メモ / Synthetic notes");
            gtk_widget_set_visible(w.preview, TRUE); gtk_widget_set_visible(w.previewHint, FALSE);
            break;
        }
        case 1:
            check(!adw_style_manager_get_dark(adw_application_get_style_manager(w.application)), "system light preference applied");
            check(rows(w.entries->widget()) > 0 && rows(w.entries->widget()) < 1000, "twenty thousand entries create a bounded set of reusable rows");
            check(!adw_overlay_split_view_get_collapsed(w.split) && adw_overlay_split_view_get_show_sidebar(w.split), "wide window shows sidebar");
            check(!gtk_list_view_get_single_click_activate(GTK_LIST_VIEW(w.entries->widget())), "single selection previews; activation requires double click or Enter");
            check(!gtk_editable_get_editable(w.previewPassword), "preview password is read only");
            check(gtk_widget_get_visible(w.preview) && !std::strcmp(gtk_label_get_text(w.previewTitle), "メール / Mail"), "preview fields visible below list");
            accentPixel();
            if (++colorIndex < G_N_ELEMENTS(colors)) { check(g_settings_set_string(settings, "accent-color", colors[colorIndex]), "private light accent change"); return G_SOURCE_CONTINUE; }
            snapshot("wide-light.png"); colorIndex = 0; g_settings_set_string(settings, "accent-color", colors[0]);
            check(g_settings_set_string(settings, "color-scheme", "prefer-dark"), "private memory-only dark setting"); break;
        case 2:
            check(adw_style_manager_get_dark(adw_application_get_style_manager(w.application)), "system dark preference applied dynamically");
            accentPixel();
            if (++colorIndex < G_N_ELEMENTS(colors)) { check(g_settings_set_string(settings, "accent-color", colors[colorIndex]), "private dark accent change"); return G_SOURCE_CONTINUE; }
            snapshot("wide-dark.png");
            gtk_window_set_default_size(w.window, 560, 620); break;
        case 3:
            check(adw_overlay_split_view_get_collapsed(w.split) && gtk_widget_get_visible(w.sidebarButton), "narrow window exposes collapsible groups");
            check(gtk_widget_get_width(GTK_WIDGET(w.window)) == 560, "minimum window width respected");
            snapshot("narrow-dark.png"); adw_overlay_split_view_set_show_sidebar(w.split, TRUE); break;
        case 4:
            check(adw_overlay_split_view_get_show_sidebar(w.split), "group sidebar opens in narrow window"); snapshot("narrow-groups.png");
            w.groups->expandGroup(0);
            check(w.groups->visibleCount() == 1200, "expansion exposes all child groups in a virtualized tree");
            w.groups->select({true, 0}); w.groups->toggleSelected();
            check(w.groups->visibleCount() == 1, "group collapses again");
            adw_overlay_split_view_set_show_sidebar(w.split, FALSE);
            w.edit({"e0", "g0", "メール / Mail", "demo@example.invalid", "synthetic-screenshot-password", "https://example.invalid", "Synthetic notes only"}); break;
        case 5:
            check(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w.window)), "editor opens at narrow width"); snapshot("narrow-editor.png");
            w.automaticLock(); break;
        case 6:
            check(!w.unlocked && w.entryRows.empty() && w.groupRows.empty() && rows(w.entries->widget()) == 0, "lock clears virtualized models and account text");
            check(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w.window)), "lock closes editor");
            check(!std::strcmp(adw_status_page_get_title(ADW_STATUS_PAGE(w.welcome)), tr("Your database is locked")), "a previously opened database has a genuine locked state");
            check(!gtk_widget_get_visible(w.preview) && !std::strlen(gtk_editable_get_text(w.previewPassword)) && !std::strlen(gtk_label_get_text(w.previewNotes)), "lock clears preview secrets and notes");
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
            w.refreshing = true; w.unlocked = true; w.selectedGroup = "g0";
            w.groupRows = {{"g0", "GNOME Demo", 0, 66, false}};
            w.groups->reset(w.groupRows, {}, false); w.groups->select({true, 0}); w.refreshing = false;
            gtk_stack_set_visible_child_name(w.stack, "unlocked"); w.sensitivity();
            w.groupName(true); break;
        case 10: {
            auto* dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w.window));
            check(dialog && !std::strcmp(adw_dialog_get_title(dialog), tr("Edit group")), "localized combined group editor opens");
            auto* field = GTK_EDITABLE(gtk_widget_get_first_child(adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(dialog))));
            check(!std::strcmp(gtk_editable_get_text(field), "GNOME Demo"), "group editor starts with current name");
            check(!std::strcmp(gtk_entry_get_placeholder_text(GTK_ENTRY(field)), tr("Group name")), "localized group name field");
            gtk_editable_set_text(field, "");
            check(!adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(dialog), "save"), "empty group name disables save");
            gtk_editable_set_text(field, "GNOME Demo");
            check(adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(dialog), "save"), "restoring group name enables save");
            break;
        }
        case 11:
            snapshot("narrow-group-editor.png");
            adw_dialog_force_close(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w.window))); break;
        case 12:
            check(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w.window)), "cancel closes combined group editor");
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
