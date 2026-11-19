// Product UI in a disposable Wayland session, with synthetic keys/databases.
#define main keepassxc_gnome_product_main
#include "../src/main.cpp"
#undef main
#include "KeyFileFixtures.h"
#include "PickerPortal.h"
#include <QTemporaryDir>

namespace {
struct KeyFileUiProbe {
    std::unique_ptr<Window> ui;
    std::unique_ptr<PickerTest::Portal> portal;
    QTemporaryDir files;
    std::string source, onlySource, keyPath, wrapper;
    std::vector<GtkWidget*> retained;
    GtkWidget *oldPassword=nullptr, *oldKeyLabel=nullptr;
    unsigned stage=0, ticks=0, checks=0, calls=0;
    bool combinedRequested=false;
    void check(bool ok,const char* label) { if (!ok) { std::fprintf(stderr,"FAIL key file UI: %s\n",label); std::exit(2); } ++checks; }
    template<class Predicate> static GtkWidget* find(GtkWidget* root,Predicate predicate) {
        if (predicate(root)) return root;
        for (auto* child=gtk_widget_get_first_child(root); child; child=gtk_widget_get_next_sibling(child))
            if (auto* match=find(child,predicate)) return match;
        return nullptr;
    }
    GtkWidget* dialog() { return GTK_WIDGET(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window))); }
    GtkWidget* button(const char* title) {
        auto* found=find(dialog(),[title](auto* widget) { return GTK_IS_BUTTON(widget) && gtk_button_get_label(GTK_BUTTON(widget)) && !std::strcmp(gtk_button_get_label(GTK_BUTTON(widget)),title); });
        check(found,"localized button present"); return found;
    }
    GtkWidget* password() { return find(dialog(),[](auto* widget){return GTK_IS_PASSWORD_ENTRY(widget);}); }
    GtkCheckButton* usePassword() { return GTK_CHECK_BUTTON(find(dialog(),[](auto* widget){return GTK_IS_CHECK_BUTTON(widget);})); }
    GtkWidget* keyLabel() {
        return find(dialog(),[](auto* widget){return GTK_IS_LABEL(widget) && (!std::strcmp(gtk_label_get_text(GTK_LABEL(widget)),tr("No key file selected")) || !std::strcmp(gtk_label_get_text(GTK_LABEL(widget)),"合成鍵.keyx"));});
    }
    void click(const char* title) { g_signal_emit_by_name(button(title),"clicked"); }
    bool selected() { return !std::strcmp(gtk_label_get_text(GTK_LABEL(keyLabel())),"合成鍵.keyx"); }
    void retain() {
        check(dialog(),"credential form exists"); retained.push_back(GTK_WIDGET(g_object_ref(dialog())));
        oldPassword=password(); oldKeyLabel=keyLabel(); check(oldPassword && oldKeyLabel,"masked password and key path label");
    }
    void cleared() {
        check(!*gtk_editable_get_text(GTK_EDITABLE(oldPassword)) && !*gtk_label_get_text(GTK_LABEL(oldKeyLabel)),"retained dialog clears password and key filename");
    }
    void fresh(const std::string& path) {
        ui->credentials(false,path); check(dialog(),"fresh form");
        check(gtk_check_button_get_active(usePassword()) && !*gtk_editable_get_text(GTK_EDITABLE(password())) && !gtk_widget_get_sensitive(button(tr("Unlock"))),"password and key are never restored");
        check(!std::strcmp(gtk_label_get_text(GTK_LABEL(keyLabel())),tr("No key file selected")),"fresh key-file selection");
    }
    void phase(const char* value) {
        const auto name=std::string(g_getenv("KPXC_UI_ROOT"))+"/keyfile-phase";
        check(g_file_set_contents(name.c_str(),value,-1,nullptr),"real-key phase notification");
    }
    void snapshot() {
        auto* widget=GTK_WIDGET(ui->window); auto* paintable=gtk_widget_paintable_new(widget); auto* shot=gtk_snapshot_new();
        gdk_paintable_snapshot(paintable,GDK_SNAPSHOT(shot),gtk_widget_get_width(widget),gtk_widget_get_height(widget));
        auto* node=gtk_snapshot_free_to_node(shot); check(node,"key form render node");
        auto* texture=gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(widget)),node,nullptr);
        const auto path=std::string(g_getenv("KPXC_UI_ROOT"))+"/keyfile-unlock.png";
        check(gdk_texture_save_to_png(texture,path.c_str()),"key-file dialog screenshot");
        g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
    }
    gboolean tick() {
        check(++ticks<300,"bounded key UI scenario"); auto& w=*ui;
        if (w.busy || !gtk_widget_get_mapped(GTK_WIDGET(w.window)) || (w.guard && !w.guard->settled())) return G_SOURCE_CONTINUE;
        switch (stage) {
        case 0:
            gtk_window_set_default_size(w.window,560,620); fresh(source); retain();
            portal->reply=PickerTest::Portal::Cancel; click(tr("Choose key file")); break;
        case 1:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            check(!gtk_widget_get_sensitive(button(tr("Unlock"))),"cancelled chooser supplies no key");
            portal->reply=PickerTest::Portal::Select; click(tr("Choose key file")); break;
        case 2:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            check(selected() && gtk_widget_get_sensitive(button(tr("Unlock"))),"selected key allows explicit empty-password factor");
            gtk_editable_set_text(GTK_EDITABLE(password()),KeyFileFixture::Master);
            check(gtk_widget_grab_focus(password()),"password Enter focus"); break;
        case 3:
            if (!combinedRequested) { snapshot(); phase("combined"); combinedRequested=true; return G_SOURCE_CONTINUE; }
            if (!w.unlocked && !w.loading && !dialog()) {
                std::fprintf(stderr,"key UI unlock status: %s\n",gtk_label_get_text(w.status)); check(false,"combined unlock response succeeds");
            }
            if (!w.unlocked) return G_SOURCE_CONTINUE;
            check(w.entryRows.size()==1 && w.recent.path()==source,"real Enter unlocks combined factors and records only DB"); cleared();
            w.automaticLock(); break;
        case 4:
            fresh(onlySource); retain();
            gtk_editable_set_text(GTK_EDITABLE(password()),"unconfirmed synthetic password");
            gtk_check_button_set_active(usePassword(),FALSE);
            check(!*gtk_editable_get_text(GTK_EDITABLE(password())) && !gtk_widget_get_sensitive(password()) && !gtk_widget_get_sensitive(button(tr("Unlock"))),"key-only toggle scrubs unused password and requires key");
            click(tr("Choose key file")); break;
        case 5:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            check(selected() && gtk_widget_get_sensitive(button(tr("Unlock"))),"key-only form can unlock");
            click(tr("Remove key file")); check(!gtk_widget_get_sensitive(button(tr("Unlock"))),"removing last factor disables unlock");
            click(tr("Choose key file")); break;
        case 6:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            portal->reply=PickerTest::Portal::Cancel; click(tr("Choose key file")); break;
        case 7:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            check(selected(),"chooser cancellation preserves deliberate prior selection");
            check(gtk_widget_grab_focus(button(tr("Unlock"))),"key-only default button focus"); phase("key-only"); break;
        case 8:
            if (!w.unlocked) return G_SOURCE_CONTINUE;
            check(w.entryRows.size()==1 && w.recent.path()==onlySource,"real Enter unlocks key-file-only DB"); cleared();
            w.automaticLock(); break;
        case 9:
            fresh(source); retain(); portal->reply=PickerTest::Portal::Select; click(tr("Choose key file")); break;
        case 10:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            gtk_editable_set_text(GTK_EDITABLE(password()),"wrong synthetic password"); click(tr("Unlock")); break;
        case 11:
            check(!w.unlocked && w.entryRows.empty() && w.recent.path()==onlySource,"wrong factors publish no model and cannot overwrite recent DB"); cleared();
            fresh(source); retain(); gtk_editable_set_text(GTK_EDITABLE(password()),"unconfirmed synthetic draft");
            portal->reply=PickerTest::Portal::Hold; calls=portal->calls; click(tr("Choose key file")); break;
        case 12:
            if (portal->calls==calls) return G_SOURCE_CONTINUE;
            check(!gtk_widget_get_sensitive(button(tr("Unlock"))),"unlock disabled while choosing key file"); w.automaticLock(); break;
        case 13:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            check(!dialog() && !w.unlocked && portal->closes==1,"lock closes key chooser and credential form"); cleared();
            fresh(source); retain(); portal->reply=PickerTest::Portal::Select; click(tr("Choose key file")); break;
        case 14:
            if (w.picker.active()) return G_SOURCE_CONTINUE;
            check(selected(),"chooser remains reusable after lock cancellation"); click(tr("Cancel")); break;
        case 15:
            cleared(); check(!dialog(),"cancel dismisses form"); fresh(source); retain(); click(tr("Cancel")); break;
        case 16:
            cleared();
            for (auto* widget:retained) g_object_unref(widget);
            retained.clear();
            w.closing=true; w.worker.shutdown(); gtk_window_destroy(w.window);
            std::printf("PASS key file GUI: %u checks, combined/key-only real Enter, chooser cancel/remove, failed credentials, fresh unlock and lock scrubbing\n",checks); std::fflush(stdout); return G_SOURCE_REMOVE;
        }
        ++stage; return G_SOURCE_CONTINUE;
    }
};
}
int main(int argc,char** argv) {
    initializeUiLocale(); QCoreApplication qt(argc,argv); KeyFileFixture::check(CoreSession::initialize(),"UI core");
    const auto locale=std::filesystem::canonical("/proc/self/exe").parent_path().parent_path()/"share/locale";
    bindtextdomain("keepassxc-gnome",locale.c_str()); bind_textdomain_codeset("keepassxc-gnome","UTF-8"); textdomain("keepassxc-gnome");
    auto* launcher=g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE|G_SUBPROCESS_FLAGS_STDERR_SILENCE)); auto parent=getpid();
    g_subprocess_launcher_set_child_setup(launcher,+[](gpointer data){const auto parent=*static_cast<pid_t*>(data);if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)_exit(2);},&parent,nullptr);
    auto* pickerBus=g_subprocess_launcher_spawn(launcher,nullptr,"dbus-daemon","--session","--nofork","--print-address=1",nullptr); g_object_unref(launcher); KeyFileFixture::check(pickerBus,"private picker bus");
    auto* input=g_data_input_stream_new(g_subprocess_get_stdout_pipe(pickerBus)); auto* address=g_data_input_stream_read_line(input,nullptr,nullptr,nullptr); KeyFileFixture::check(address,"picker address");
    KeyFileUiProbe probe; probe.portal=std::make_unique<PickerTest::Portal>(address); probe.portal->keyFile=true; probe.portal->wayland=true; probe.portal->expectedTitle=tr("Choose key file");
    const auto root=std::string(g_getenv("KPXC_UI_ROOT")); probe.wrapper=root+"/keyfile-picker";
    const auto helper=std::filesystem::canonical("/proc/self/exe").parent_path()/"keepassxc-gnome-picker";
    auto* bus=g_shell_quote(address); auto* exe=g_shell_quote(helper.c_str()); const auto script=std::string("#!/bin/sh\nDBUS_SESSION_BUS_ADDRESS=")+bus+" exec "+exe+" \"$@\"\n";
    probe.check(g_file_set_contents(probe.wrapper.c_str(),script.c_str(),-1,nullptr)&&!g_chmod(probe.wrapper.c_str(),0700),"synthetic chooser wrapper");
    g_free(bus);g_free(exe);g_free(address);g_object_unref(input);
    probe.keyPath=probe.files.filePath("合成鍵.keyx").toStdString(); KeyFileFixture::write(QString::fromStdString(probe.keyPath),KeyFileFixture::keyBytes(0));
    probe.source=probe.files.filePath("combined.kdbx").toStdString();probe.onlySource=probe.files.filePath("key-only.kdbx").toStdString();
    KeyFileFixture::create(QString::fromStdString(probe.source),QString::fromStdString(probe.keyPath),"password",KeePass2::FILE_VERSION_4_1);
    KeyFileFixture::create(QString::fromStdString(probe.onlySource),QString::fromStdString(probe.keyPath),"none",KeePass2::FILE_VERSION_3_1);
    auto* uri=g_filename_to_uri(probe.keyPath.c_str(),nullptr,nullptr);probe.portal->selectedUri=uri;g_free(uri);
    auto* app=adw_application_new("org.keepassxc.Gnome.KeyFileTest",G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app,"activate",G_CALLBACK(+[](AdwApplication* app,gpointer data){auto& probe=*static_cast<KeyFileUiProbe*>(data);probe.ui=std::make_unique<Window>(app,probe.wrapper);probe.ui->present();g_timeout_add(350,+[](gpointer p){return static_cast<KeyFileUiProbe*>(p)->tick();},&probe);}),&probe);
    const auto result=g_application_run(G_APPLICATION(app),1,argv);probe.ui.reset();g_object_unref(app);probe.portal.reset();g_subprocess_force_exit(pickerBus);g_subprocess_wait(pickerBus,nullptr,nullptr);g_object_unref(pickerBus);g_unlink(probe.wrapper.c_str());return result;
}
