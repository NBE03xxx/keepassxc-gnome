// Product UI, separate test executable, synthetic data and private XDG state.
#define main keepassxc_gnome_product_main
#include "../src/main.cpp"
#undef main
#include <QTemporaryDir>
#include <cstring>

namespace {
struct WelcomeUiProbe {
    AdwApplication* app;
    std::unique_ptr<Window> ui;
    QTemporaryDir files;
    std::string source, target, copy;
    unsigned stage = 0, ticks = 0, checks = 0;
    explicit WelcomeUiProbe(AdwApplication* app) : app(app), files(QString::fromLocal8Bit(g_getenv("KPXC_UI_ROOT")) + "/welcome.XXXXXX") {
        check(files.isValid(), "private fixture folder");
        source = files.filePath("メール & <accounts>.kdbx").toStdString();
        target = files.filePath("second.kdbx").toStdString(); copy = files.filePath("copy.kdbx").toStdString();
        check(CoreSession::createFixture(source, FixtureFormat::Kdbx3), "synthetic upstream-encrypted database");
    }
    void check(bool ok, const char* label) { if (!ok) { std::fprintf(stderr, "FAIL welcome UI: %s\n", label); std::exit(2); } ++checks; }
    AdwAlertDialog* dialog() { return ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window))); }
    static GtkWidget* find(GtkWidget* root, GType type) {
        if (G_TYPE_CHECK_INSTANCE_TYPE(root, type)) return root;
        for (auto* child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
            if (auto* match = find(child, type)) return match;
        return nullptr;
    }
    static GtkWidget* responseButton(GtkWidget* root, const char* label) {
        if (GTK_IS_BUTTON(root) && gtk_button_get_label(GTK_BUTTON(root)) && !std::strcmp(gtk_button_get_label(GTK_BUTTON(root)), label)) return root;
        for (auto* child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
            if (auto* match = responseButton(child, label)) return match;
        return nullptr;
    }
    void answer(const char* password) {
        check(dialog(), "fresh credential dialog");
        auto* field = find(GTK_WIDGET(dialog()), GTK_TYPE_PASSWORD_ENTRY); check(field, "protected password entry");
        check(!*gtk_editable_get_text(GTK_EDITABLE(field)), "no password is restored from metadata");
        auto* text = find(field, GTK_TYPE_TEXT); check(text && !gtk_text_get_visibility(GTK_TEXT(text)), "password is masked");
        gtk_editable_set_text(GTK_EDITABLE(field), password);
        auto* unlock = responseButton(GTK_WIDGET(dialog()), tr("Unlock")); check(unlock, "actual unlock button"); g_signal_emit_by_name(unlock, "clicked");
    }
    void restart() {
        auto old = std::move(ui); ui = std::make_unique<Window>(app);
        old->closing = true; old->worker.shutdown(); gtk_window_destroy(old->window); old.reset(); ui->present();
    }
    void snapshot(const char* name) {
        auto* widget = GTK_WIDGET(ui->window); auto* paintable = gtk_widget_paintable_new(widget); auto* shot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(shot), gtk_widget_get_width(widget), gtk_widget_get_height(widget));
        auto* node = gtk_snapshot_free_to_node(shot); check(node, "rendered frame");
        auto* texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(widget)), node, nullptr);
        auto* path = g_build_filename(g_getenv("KPXC_UI_ROOT"), name, nullptr);
        check(gdk_texture_save_to_png(texture, path), "actual GTK screenshot");
        g_free(path); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
    }
    void checkFlavor(GtkWidget* widget) {
        if (ADW_IS_BANNER(widget)) {
            const bool development = !std::strcmp(adw_banner_get_title(ADW_BANNER(widget)), tr("Development build. Use synthetic test databases only."));
            if (development) check(KPXC_GNOME_DEVELOPMENT_UI, "public UI has no development banner");
        }
        if (!KPXC_GNOME_DEVELOPMENT_UI && GTK_IS_LABEL(widget)) {
            const std::string text = gtk_label_get_text(GTK_LABEL(widget));
            check(text.find("検証") == std::string::npos && text.find("test database") == std::string::npos, "public welcome has ordinary user-facing text");
        }
        for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) checkFlavor(child);
    }
    gboolean tick() {
        check(++ticks < 400, "bounded workflow"); auto& w = *ui;
        if (w.busy || (w.guard && !w.guard->settled()) || !gtk_widget_get_mapped(GTK_WIDGET(w.window))) return G_SOURCE_CONTINUE;
        switch (stage) {
        case 0:
            check(w.recent.path().empty() && !w.unlocked, "first start has no history or open database");
            check(!std::strcmp(adw_status_page_get_title(ADW_STATUS_PAGE(w.welcome)), tr("Welcome to KeePassXC GNOME")), "localized welcome");
            check(gtk_widget_get_mapped(w.welcomeNew) && gtk_widget_get_mapped(w.welcomeOpen), "central create and open buttons visible");
            checkFlavor(GTK_WIDGET(w.window)); snapshot("welcome.png");
            { auto* file = g_file_new_for_path(source.c_str()); w.openFiles(&file, 1); g_object_unref(file); }
            check(w.openRequestTimer && w.recent.path().empty(), "file handoff is deferred without recording it");
            g_signal_emit_by_name(w.welcomeNew, "clicked");
            check(!w.openRequestTimer, "manual New cancels an unstarted file handoff"); break;
        case 1:
            check(dialog() && !std::strcmp(adw_dialog_get_title(ADW_DIALOG(dialog())), tr("New database")), "central create invokes actual product form");
            adw_dialog_close(ADW_DIALOG(dialog())); break;
        case 2: w.credentials(false, source); break;
        case 3: answer("wrong-synthetic-password"); break;
        case 4:
            check(!w.unlocked && w.recent.path().empty(), "failed password never records a DB"); w.credentials(false, source); break;
        case 5: answer(CoreSession::FixturePassword); break;
        case 6:
            check(w.unlocked && w.currentPath == source && w.recent.path() == source, "successful unlock associates and remembers only its source");
            check(!std::strcmp(adw_window_title_get_title(w.windowTitle), "メール & <accounts>.kdbx"), "header shows literal basename, no directory or markup");
            snapshot("filename.png"); w.saveDatabase(copy, false); break;
        case 7:
            check(w.currentPath == source && w.recent.path() == source, "copy save does not replace current or recent DB"); w.saveDatabase(copy, true); break;
        case 8:
            check(w.currentPath == source && w.recent.path() == source, "failed Save As leaves file association intact"); w.saveDatabase(target, true); break;
        case 9:
            check(w.currentPath == target && w.recent.path() == target, "successful Save As becomes the recent DB");
            check(RecentDatabase().path() == target, "successful save persisted only metadata"); w.lock(); break;
        case 10:
            check(!w.unlocked && w.currentPath.empty() && w.entryRows.empty() && w.groupRows.empty(), "lock clears live filename and model");
            check(!std::strcmp(adw_status_page_get_title(ADW_STATUS_PAGE(w.welcome)), tr("Unlock your database")), "recent-file locked invitation");
            snapshot("recent.png"); restart(); break;
        case 11:
            check(!w.unlocked && w.currentPath.empty() && w.recent.path() == target, "restart shows recent metadata without unlocking");
            check(gtk_widget_get_mapped(w.recentButton) && !std::strcmp(gtk_label_get_text(w.recentLabel), "second.kdbx"), "central unlock and recent basename");
            g_signal_emit_by_name(w.recentButton, "clicked"); break;
        case 12:
            check(dialog() && !std::strcmp(adw_alert_dialog_get_body(dialog()), "second.kdbx"), "unlock identifies recent DB"); answer(CoreSession::FixturePassword); break;
        case 13:
            check(w.unlocked && w.currentPath == target, "central recent button unlocks correct DB with fresh password"); w.lock(); break;
        case 14:
            g_signal_emit_by_name(w.forgetButton, "clicked");
            check(w.recent.path().empty() && RecentDatabase().path().empty(), "forget clears only metadata");
            check(std::filesystem::exists(target), "forget does not delete database"); restart(); break;
        case 15:
            check(w.recent.path().empty() && !std::strcmp(adw_status_page_get_title(ADW_STATUS_PAGE(w.welcome)), tr("Welcome to KeePassXC GNOME")), "restart after forgetting returns to welcome");
            gtk_window_set_default_size(w.window, 560, 620); break;
        case 16:
            check(gtk_widget_get_width(GTK_WIDGET(w.window)) == 560, "narrow welcome respects minimum width");
            check(gtk_widget_get_allocated_width(w.welcomeOpen) >= 240 && gtk_widget_get_height(w.welcomeOpen) > 0, "central actions retain usable size");
            check(!gtk_widget_get_visible(w.sidebarButton), "welcome has no irrelevant group-sidebar toggle");
            snapshot("welcome-narrow.png");
            { auto* file = g_file_new_for_path(source.c_str()); w.openFiles(&file, 1); g_object_unref(file); }
            check(w.openRequestTimer, "pending handoff exists at window teardown");
            w.closing = true; w.worker.shutdown(); gtk_window_destroy(w.window);
            std::printf("PASS welcome GUI: %u checks, flavor=%s\n", checks, KPXC_GNOME_DEVELOPMENT_UI ? "development" : "public"); std::fflush(stdout); return G_SOURCE_REMOVE;
        }
        ++stage; return G_SOURCE_CONTINUE;
    }
};
}
int main(int argc, char** argv) {
    initializeUiLocale(); const auto locale = localeDirectory(); bindtextdomain("keepassxc-gnome", locale.c_str());
    bind_textdomain_codeset("keepassxc-gnome", "UTF-8"); textdomain("keepassxc-gnome");
    QCoreApplication qt(argc, argv); if (!CoreSession::initialize()) return 1;
    if (argc == 3 && std::string(argv[1]) == "--write-fixture") return CoreSession::createFixture(argv[2], FixtureFormat::Kdbx3) ? 0 : 1;
    auto* app = adw_application_new("org.keepassxc.Gnome.WelcomeTest", G_APPLICATION_DEFAULT_FLAGS);
    WelcomeUiProbe probe(app);
    g_signal_connect(app, "activate", G_CALLBACK(+[](AdwApplication*, gpointer data) {
        auto& probe = *static_cast<WelcomeUiProbe*>(data); probe.ui = std::make_unique<Window>(probe.app); probe.ui->present();
        g_timeout_add(400, +[](gpointer data) { return static_cast<WelcomeUiProbe*>(data)->tick(); }, &probe);
    }), &probe);
    const auto result = g_application_run(G_APPLICATION(app), 1, argv); probe.ui.reset();
    const auto until = g_get_monotonic_time() + 150000;
    do { while (g_main_context_iteration(nullptr, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < until);
    g_object_unref(app); return result;
}
