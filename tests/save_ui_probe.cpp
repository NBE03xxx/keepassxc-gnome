// Compile the same Window code into a separate integration-test executable.
// No test switches, fixture controls or extra RPCs enter the product binary.
#define main keepassxc_gnome_product_main
#include "../src/main.cpp"
#undef main
#include "gnome/SafeSave.h"
#include <QTemporaryDir>
#include <QFile>

namespace {
struct SaveUiProbe {
    AdwApplication* app;
    std::unique_ptr<Window> ui;
    QTemporaryDir files;
    std::string source, target, copy, recovered, backup;
    QByteArray original, afterFirstSave, external;
    unsigned stage = 0, ticks = 0, recordCount = 0;
    explicit SaveUiProbe(AdwApplication* app) : app(app), files(QString::fromLocal8Bit(g_getenv("KPXC_UI_ROOT")) + "/save.XXXXXX") {
        check(files.isValid(), "private test directory");
        source = files.filePath("source.kdbx").toStdString(); target = files.filePath("active.kdbx").toStdString();
        copy = files.filePath("copy.kdbx").toStdString(); recovered = files.filePath("recovered.kdbx").toStdString();
        check(CoreSession::createFixture(source, FixtureFormat::Kdbx3), "synthetic fixture"); original = load(source);
    }
    static void check(bool ok, const char* label) { if (!ok) { std::fprintf(stderr, "FAIL save UI: %s\n", label); std::exit(2); } }
    static QByteArray load(const std::string& path) { QFile file(QString::fromStdString(path)); check(file.open(QIODevice::ReadOnly), "read test data"); return file.readAll(); }
    static GtkWidget* find(GtkWidget* root, GType type, const char* text = nullptr) {
        if (G_TYPE_CHECK_INSTANCE_TYPE(root, type)) {
            const char* label = GTK_IS_BUTTON(root) ? gtk_button_get_label(GTK_BUTTON(root)) : nullptr;
            if (!text || (label && !std::strcmp(label, text))) return root;
        }
        for (auto* child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child))
            if (auto* match = find(child, type, text)) return match;
        return nullptr;
    }
    AdwDialog* dialog() { return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window)); }
    bool click(const char* text) {
        auto* visible = dialog(); if (!visible) return false;
        auto* button = find(GTK_WIDGET(visible), GTK_TYPE_BUTTON, text); if (!button) return false;
        check(gtk_widget_get_sensitive(button), "dialog action enabled"); g_signal_emit_by_name(button, "clicked"); return true;
    }
    void edit(const char* title) {
        ui->submit([title](RemoteSession& session) { return session.rename(session.entries().front().id, title); }, [this](bool ok) {
            check(ok, "edit synthetic data"); ui->dirty = true; ui->refresh();
        });
    }
    bool status(const char* expected) { return !std::strcmp(gtk_label_get_text(ui->status), tr(expected)); }
    gboolean tick() {
        check(++ticks < 1600, "bounded GUI workflow");
        auto& w = *ui;
        if (w.busy) return G_SOURCE_CONTINUE;
        switch (stage) {
        case 0:
            if (w.guard && !w.guard->settled()) return G_SOURCE_CONTINUE;
            w.submit([this](RemoteSession& session) { return session.unlock(source, CoreSession::FixturePassword); }, [this](bool ok) {
                check(ok, "open fixture"); ui->hasOpenedDatabase = ui->unlocked = ui->sourceKnown = true;
                gtk_stack_set_visible_child_name(ui->stack, "unlocked"); ui->refresh();
            }); break;
        case 1: edit("Synthetic first GUI save"); break;
        case 2: w.saveDatabase(target, true); break;
        case 3:
            check(status("Database saved.") && !w.dirty && w.sourceKnown, "Save As publishes verified clean state");
            check(w.currentPath == target && w.recent.path() == target, "associated save updates title path and recent file");
            afterFirstSave = load(target); edit("Synthetic second GUI save"); break;
        case 4: w.saveDatabase(copy, false); break;
        case 5:
            check(w.dirty && status("Copy saved. The current database still has its previous save state.") && load(target) == afterFirstSave,
                "copy save leaves dirty state and original source");
            check(w.currentPath == target && w.recent.path() == target, "copy save never replaces current or recent filename");
            g_signal_emit_by_name(w.saveButton, "clicked"); break;
        case 6:
            check(status("Database saved.") && !w.dirty && load(target) != afterFirstSave, "header Save follows active source");
            external = load(target) + QByteArray("external update");
            { QFile file(QString::fromStdString(target)); check(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(external) == external.size(), "external update fixture"); }
            edit("Synthetic conflict GUI save"); break;
        case 7: g_signal_emit_by_name(w.saveButton, "clicked"); break;
        case 8:
            check(w.dirty && status("The destination changed outside this application. Review recovery files and save your changes under a new name.")
                && load(target) == external, "external update message retains dirty changes");
            w.inspectRecovery(files.path().toStdString()); break;
        case 9: {
            if (!dialog() || !find(GTK_WIDGET(dialog()), ADW_TYPE_EXPANDER_ROW)) return G_SOURCE_CONTINUE;
            auto* row = ADW_EXPANDER_ROW(find(GTK_WIDGET(dialog()), ADW_TYPE_EXPANDER_ROW));
            adw_expander_row_set_expanded(row, TRUE);
            check(find(GTK_WIDGET(dialog()), GTK_TYPE_BUTTON, tr("Open copy")), "recovery dialog contains copy actions");
            bool complete; const auto records = SafeSave::recoveries(files.path(), complete); recordCount = records.size();
            check(complete && recordCount >= 3, "recovery records listed");
            for (const auto& record : records) for (const auto& file : record.files)
                if (file.kind == "previous.kdbx") backup = file.path;
            check(!backup.empty(), "original backup present");
            // Use the actual Open copy button, not a direct backend unlock.
            auto* button = find(GTK_WIDGET(dialog()), GTK_TYPE_BUTTON, tr("Open copy"));
            g_signal_emit_by_name(button, "clicked"); break;
        }
        case 10: if (!click(tr("Discard"))) return G_SOURCE_CONTINUE; break;
        case 11: {
            if (!dialog() || !ADW_IS_ALERT_DIALOG(dialog())) return G_SOURCE_CONTINUE;
            auto* password = find(GTK_WIDGET(dialog()), GTK_TYPE_PASSWORD_ENTRY); if (!password) return G_SOURCE_CONTINUE;
            gtk_editable_set_text(GTK_EDITABLE(password), CoreSession::FixturePassword);
            if (!click(tr("Unlock"))) return G_SOURCE_CONTINUE;
            break;
        }
        case 12:
            check(w.unlocked && !w.sourceKnown && w.dirty && !w.activeRecovery.empty(), "opened recovery requires Save As and discard confirmation");
            check(w.currentPath.empty() && w.recent.path() == target, "recovery copy is not remembered as the source database");
            w.saveDatabase(recovered, true); break;
        case 13:
            check(!w.dirty && w.activeRecovery.empty() && status("Database saved."), "recovered copy saves to new file");
            w.inspectRecovery(files.path().toStdString()); break;
        case 14:
            if (!dialog() || !find(GTK_WIDGET(dialog()), GTK_TYPE_BUTTON, tr("Remove record"))) return G_SOURCE_CONTINUE;
            check(click(tr("Remove record")), "recovery removal requests confirmation"); break;
        case 15: if (!click(tr("Remove"))) return G_SOURCE_CONTINUE; break;
        case 16: {
            if (!dialog() || !find(GTK_WIDGET(dialog()), ADW_TYPE_EXPANDER_ROW)) return G_SOURCE_CONTINUE;
            bool complete; auto records = SafeSave::recoveries(files.path(), complete);
            check(complete && records.size() == recordCount, "new recovery save adds one record and confirmed removal deletes one");
            check(load(source) == original && load(target) == external && QFile::exists(QString::fromStdString(recovered)), "recovery never overwrites either original");
            std::puts("PASS save GUI: Save As, copy dirty state, header Save, conflict message, recovery rows, Open copy, unlock, recovered Save As and confirmed removal");
            w.closing = true; w.worker.cancel(); w.conceal(); gtk_window_destroy(w.window); g_application_quit(G_APPLICATION(app));
            return G_SOURCE_REMOVE;
        }
        }
        ++stage;
        return G_SOURCE_CONTINUE;
    }
};
}
int main(int argc, char** argv)
{
    initializeUiLocale();
    const auto locale = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path() / "share/locale";
    bindtextdomain("keepassxc-gnome", locale.c_str()); bind_textdomain_codeset("keepassxc-gnome", "UTF-8"); textdomain("keepassxc-gnome");
    QCoreApplication qt(argc, argv); SaveUiProbe::check(CoreSession::initialize(), "crypto");
    auto* app = adw_application_new("org.keepassxc.Gnome.SaveTest", G_APPLICATION_DEFAULT_FLAGS);
    SaveUiProbe probe(app);
    g_signal_connect(app, "activate", G_CALLBACK(+[](AdwApplication* app, gpointer data) {
        auto& probe = *static_cast<SaveUiProbe*>(data); probe.ui = std::make_unique<Window>(app); probe.ui->present();
        g_timeout_add(25, +[](gpointer data) { return static_cast<SaveUiProbe*>(data)->tick(); }, &probe);
    }), &probe);
    const auto result = g_application_run(G_APPLICATION(app), 1, argv);
    probe.ui.reset(); g_object_unref(app);
    return result;
}
