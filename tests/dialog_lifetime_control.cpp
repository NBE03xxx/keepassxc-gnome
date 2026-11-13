// Isolate present/force-close ordering with public libadwaita APIs only.
#include <adwaita.h>
#include <array>
#include <clocale>
#include <cstdio>
#include <cstring>
static unsigned finalizedDialogs=0;
static unsigned finalizedBreakpoints=0;
struct DialogProbe {
    AdwApplication* app=nullptr;
    GtkWidget* window=nullptr;
    std::array<gpointer,20> dialogs{};
    unsigned created=0;
    bool mapped=false;
    unsigned live() const { unsigned n=0;for(auto* p:dialogs)if(p)++n;return n; }
    void activate() {
        window=adw_application_window_new(GTK_APPLICATION(app));
        gtk_window_set_default_size(GTK_WINDOW(window),980,660);gtk_window_present(GTK_WINDOW(window));
        g_timeout_add(100,+[](gpointer data)->gboolean {
            auto& p=*static_cast<DialogProbe*>(data);
            auto* d=ADW_DIALOG(adw_alert_dialog_new("Synthetic dialog lifetime control",nullptr));
            g_object_set_data_full(G_OBJECT(d),"control-finalize-count",&finalizedDialogs,
                +[](gpointer data){++*static_cast<unsigned*>(data);});
            auto* breakpoint=adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600px"));
            g_object_set_data_full(G_OBJECT(breakpoint),"control-finalize-count",&finalizedBreakpoints,
                +[](gpointer data){++*static_cast<unsigned*>(data);});
            adw_dialog_add_breakpoint(d,breakpoint); // transfer full
            adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d),"cancel","Cancel");
            p.dialogs[p.created]=d;g_object_add_weak_pointer(G_OBJECT(d),&p.dialogs[p.created]);++p.created;
            adw_dialog_present(d,p.window);
            if(p.mapped)gtk_widget_add_tick_callback(GTK_WIDGET(d),+[](GtkWidget* d,GdkFrameClock*,gpointer data)->gboolean {
                auto& ticks=*static_cast<unsigned*>(data);
                if(++ticks<3)return G_SOURCE_CONTINUE;
                adw_dialog_force_close(ADW_DIALOG(d));return G_SOURCE_REMOVE;
            },new unsigned(0),+[](gpointer data){delete static_cast<unsigned*>(data);});
            else adw_dialog_force_close(d);
            if(p.created<p.dialogs.size())return G_SOURCE_CONTINUE;
            g_timeout_add(2000,+[](gpointer data)->gboolean {
                auto& p=*static_cast<DialogProbe*>(data);
                auto* list=adw_application_window_get_dialogs(ADW_APPLICATION_WINDOW(p.window));
                std::printf("DIALOG CONTROL mode=%s created=%u live_before_window_close=%u host_items=%u\n",
                    p.mapped?"after-three-frames":"immediate",p.created,p.live(),g_list_model_get_n_items(list));g_object_unref(list);
                gtk_window_destroy(GTK_WINDOW(p.window));g_application_quit(G_APPLICATION(p.app));return G_SOURCE_REMOVE;
            },&p);
            return G_SOURCE_REMOVE;
        },this);
    }
};
int main(int argc,char** argv) {
    std::setlocale(LC_ALL,"");DialogProbe p;
    p.mapped=argc>1 && std::strcmp(argv[1],"--mapped")==0;
    p.app=adw_application_new("org.keepassxc.Gnome.DialogLifetimeControl",G_APPLICATION_NON_UNIQUE);
    g_signal_connect(p.app,"activate",G_CALLBACK(+[](AdwApplication*,gpointer data){static_cast<DialogProbe*>(data)->activate();}),&p);
    const auto result=g_application_run(G_APPLICATION(p.app),1,argv);g_object_unref(p.app);
    std::printf("PASS GTK control: dialog lifetime observed; live_after_window_close=%u finalized=%u/%u breakpoint_finalized=%u/%u\n",p.live(),finalizedDialogs,p.created,finalizedBreakpoints,p.created);std::fflush(stdout);
    // Weak-pointer targets must outlive any object still held by GTK globals.
    for(auto& d:p.dialogs)if(d)g_object_remove_weak_pointer(G_OBJECT(d),&d);
    return result;
}
