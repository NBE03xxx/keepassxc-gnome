// Same product Window, driven by actual keys in a disposable GNOME session.
#define main keepassxc_gnome_product_main
#include "../src/main.cpp"
#undef main
#include <QTemporaryDir>

namespace {
struct DefaultKeysUiProbe {
    AdwApplication* app;
    std::unique_ptr<Window> ui;
    QTemporaryDir files;
    std::string target;
    unsigned stage = 0, ticks = 0, checks = 0;
    explicit DefaultKeysUiProbe(AdwApplication* app) : app(app), files(QString::fromLocal8Bit(g_getenv("KPXC_UI_ROOT")) + "/default-keys.XXXXXX") {
        check(files.isValid(), "private files"); target = files.filePath("keyboard.kdbx").toStdString();
    }
    void check(bool ok, const char* text) { if (!ok) { std::fprintf(stderr, "FAIL default keys UI: %s\n", text); std::exit(2); } ++checks; }
    bool dialog() { return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window)); }
    void ready(const char* phase) {
        const auto file = std::string(g_getenv("KPXC_UI_ROOT")) + "/default-keys-phase";
        check(g_file_set_contents(file.c_str(), phase, -1, nullptr), "test stage coordination");
        std::fprintf(stderr, "READY default keys: %s\n", phase);
    }
    static GtkWidget* notesExpander(GtkWidget* root) {
        if (ADW_IS_EXPANDER_ROW(root) && !std::strcmp(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(root)), tr("Notes"))) return root;
        for (auto* child=gtk_widget_get_first_child(root);child;child=gtk_widget_get_next_sibling(child))
            if (auto* result=notesExpander(child)) return result;
        return nullptr;
    }
    void editFor(const char* phase) {
        check(!ui->entryRows.empty(), "entry remains in selected root"); const auto id = ui->entryRows.front().id;
        ui->submit([id](RemoteSession& s) { EntryDetails d; s.entry(id, d); return d; }, [this, phase](EntryDetails& d) {
            check(!d.id.empty(), "read actual encrypted model entry"); ui->edit(d);
            if (!std::strcmp(phase,"notes")) {
                auto* dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window));
                auto* row=notesExpander(GTK_WIDGET(dialog));check(row,"prepare expanded notes");adw_expander_row_set_expanded(ADW_EXPANDER_ROW(row),TRUE);
            }
            ready(phase);
        });
    }
    gboolean tick() {
        check(++ticks < 2400, "bounded real-key workflow"); auto& w = *ui;
        if (w.busy || (!w.closing && w.guard && !w.guard->settled())) return G_SOURCE_CONTINUE;
        switch (stage) {
        case 0: w.newDatabase(); ready("create"); break;
        case 1:
            if (!w.unlocked || dialog()) return G_SOURCE_CONTINUE;
            check(w.groupRows.front().name == "Keyboard DB", "Enter creates named database");
            w.edit({"", w.selectedGroup, "", "", "", "", ""}); ready("new-entry"); break;
        case 2:
            if (dialog()) return G_SOURCE_CONTINUE;
            check(w.entryRows.size()==1 && w.entryRows.front().title=="Keyboard entry", "title Enter commits new entry once"); editFor("edit-username"); break;
        case 3:
            if (dialog()) return G_SOURCE_CONTINUE;
            check(w.entryRows.front().username=="second-user", "username Enter applies edit"); editFor("edit-password"); break;
        case 4: {
            if (dialog()) return G_SOURCE_CONTINUE;
            const auto id = w.entryRows.front().id;
            w.submit([id](RemoteSession& s) { EntryDetails d; s.entry(id,d); return d; }, [this](EntryDetails& d) {
                check(d.password=="keyboard-changed-secret", "masked password Enter applies edit"); ui->edit(d); ready("edit-url");
            }); break;
        }
        case 5: {
            if (dialog()) return G_SOURCE_CONTINUE;
            const auto id = w.entryRows.front().id;
            w.submit([id](RemoteSession& s) { EntryDetails d; s.entry(id,d); return d; }, [this](EntryDetails& d) {
                check(d.url=="https://keyboard.example.invalid/edited", "URL Enter applies edit"); ui->groupName(false); ready("new-group");
            }); break;
        }
        case 6: {
            if (dialog()) return G_SOURCE_CONTINUE;
            const auto found=std::find_if(w.groupRows.begin(),w.groupRows.end(),[](const auto& g){return g.name=="Keyboard group";});
            check(found!=w.groupRows.end(), "Enter creates group"); w.refreshing=true;
            w.selectedGroup=found->id; w.groups->expandGroup(0); w.groups->select({true,static_cast<guint>(found-w.groupRows.begin())}); w.refreshing=false;
            w.groupName(true); ready("edit-group"); break;
        }
        case 7:
            if (dialog()) return G_SOURCE_CONTINUE;
            check(std::any_of(w.groupRows.begin(),w.groupRows.end(),[](const auto& g){return g.name=="Keyboard group renamed";}), "Enter renames selected group");
            w.selectedGroup=w.groupRows.front().id; w.refresh(); break;
        case 8: editFor("notes"); break;
        case 9: {
            if (dialog()) return G_SOURCE_CONTINUE;
            const auto id=w.entryRows.front().id;
            w.submit([id](RemoteSession& s) { EntryDetails d;s.entry(id,d);return d; },[this](EntryDetails& d) {
                check(d.notes=="first line\n", "plain Enter in notes stays a newline"); ui->saveDatabase(target,true);
            }); break;
        }
        case 10:
            check(w.sourceKnown && !w.dirty && w.currentPath==target, "associate real encrypted test file"); editFor("edit-before-close"); break;
        case 11:
            if (dialog()) return G_SOURCE_CONTINUE;
            check(w.dirty && w.entryRows.front().title=="Keyboard final", "final title Enter leaves an unsaved DB change"); gtk_window_close(w.window); ready("close-save"); break;
        case 12: {
            if (!w.closing) return G_SOURCE_CONTINUE;
            CoreSession verify; check(verify.unlock(target,"keyboard-master"), "Enter-driven close saves a readable KDBX");
            const auto rows=verify.entries();check(rows.size()==1 && rows.front().title=="Keyboard final" && rows.front().username=="second-user", "file contains confirmed keyboard edits");
            EntryDetails d;check(verify.entry(rows.front().id,d) && d.password=="keyboard-changed-secret" && d.notes=="first line\n", "password and notes round trip");Sensitive::wipe(d);
            std::printf("PASS default keys GUI: %u checks; create, entry fields, group create/edit, notes newline, focused auxiliary action and close-save\n",checks);std::fflush(stdout);
            g_application_release(G_APPLICATION(app));return G_SOURCE_REMOVE;
        }
        }
        ++stage; return G_SOURCE_CONTINUE;
    }
};
}
int main(int argc,char** argv) {
    initializeUiLocale(); const auto locale=localeDirectory();bindtextdomain("keepassxc-gnome",locale.c_str());bind_textdomain_codeset("keepassxc-gnome","UTF-8");textdomain("keepassxc-gnome");
    QCoreApplication qt(argc,argv);if(!CoreSession::initialize())return 1;
    auto* app=adw_application_new("org.keepassxc.Gnome.DefaultKeysTest",G_APPLICATION_DEFAULT_FLAGS);DefaultKeysUiProbe probe(app);
    g_signal_connect(app,"activate",G_CALLBACK(+[](AdwApplication* app,gpointer data){
        auto& p=*static_cast<DefaultKeysUiProbe*>(data);g_application_hold(G_APPLICATION(app));p.ui=std::make_unique<Window>(app);p.ui->present();
        g_timeout_add(50,+[](gpointer data){return static_cast<DefaultKeysUiProbe*>(data)->tick();},&p);
    }),&probe);
    const auto result=g_application_run(G_APPLICATION(app),1,argv);probe.ui.reset();g_object_unref(app);return result;
}
