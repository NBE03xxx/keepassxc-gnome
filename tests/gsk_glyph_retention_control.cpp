// Accelerated renderer contrast. No database/model/IPC/clipboard is linked.
// This deliberately clears more often than the product; never a timing proof.
#include "gnome/FontCache.h"
#include "SoakMetrics.h"
#include <adwaita.h>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <cstdlib>

struct RendererProbe {
    AdwApplication* app=nullptr;
    GtkWidget *window=nullptr,*fixed=nullptr,*label=nullptr;
    unsigned frames=0,idle=0,idleLimit=60;
    SoakMetrics metrics;
    explicit RendererProbe(const char* root):metrics(root) {}
    void activate() {
        window=adw_application_window_new(GTK_APPLICATION(app));
        fixed=gtk_fixed_new();
        label=gtk_label_new("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 日本語の描画検証");
        gtk_fixed_put(GTK_FIXED(fixed),label,12,24);
        adw_application_window_set_content(ADW_APPLICATION_WINDOW(window),fixed);
        gtk_window_set_default_size(GTK_WINDOW(window),1200,200);
        gtk_window_present(GTK_WINDOW(window));
        gtk_widget_add_tick_callback(label,+[](GtkWidget*,GdkFrameClock*,gpointer data)->gboolean {
            auto& p=*static_cast<RendererProbe*>(data);++p.frames;
            const float scale=.9f+float(p.frames)/6000.f;
            auto* transform=gsk_transform_scale(nullptr,scale,scale);
            gtk_fixed_set_child_transform(GTK_FIXED(p.fixed),p.label,transform);gsk_transform_unref(transform);
            if(p.frames%100==0) {
                auto* map=pango_context_get_font_map(gtk_widget_get_pango_context(p.label));
                if(!FontCache::clear(map))std::abort();
                ++p.metrics.fontCacheClears;
            }
            if(p.frames%300==0) {
                std::fprintf(stderr,"RENDER CONTROL frames=%u clears=%u\n",p.frames,p.metrics.fontCacheClears);
                p.metrics.sample("animated",p.frames);
            }
            if(p.frames<1200)return G_SOURCE_CONTINUE;
            std::fprintf(stderr,"RENDER CONTROL idle starts; no more scale changes\n");
            g_timeout_add_seconds(1,+[](gpointer data)->gboolean {
                auto& p=*static_cast<RendererProbe*>(data);++p.idle;
                if(p.idle%10==0)p.metrics.sample("idle",p.frames);
                gtk_widget_queue_draw(p.label);
                if(p.idle<p.idleLimit)return G_SOURCE_CONTINUE;
                gtk_window_destroy(GTK_WINDOW(p.window));
                std::printf("PASS GTK control: 1200 unique frame scales, 12 public cache clears, %us idle\n",p.idleLimit);std::fflush(stdout);
                g_application_quit(G_APPLICATION(p.app));return G_SOURCE_REMOVE;
            },&p);
            return G_SOURCE_REMOVE;
        },this,nullptr);
    }
};
int main(int argc,char** argv) {
    std::setlocale(LC_ALL,"");
    const auto* root=g_getenv("KPXC_UI_ROOT");if(!root)return 2;
    RendererProbe p(root);
    if(argc==3 && !std::strcmp(argv[1],"--idle")) {
        char* end=nullptr;const auto value=std::strtoul(argv[2],&end,10);
        if(!end || *end || value<15 || value>180)return 2;
        p.idleLimit=unsigned(value);
    } else if(argc!=1)return 2;
    p.app=adw_application_new("org.keepassxc.Gnome.RendererControl",G_APPLICATION_NON_UNIQUE);
    g_signal_connect(p.app,"activate",G_CALLBACK(+[](AdwApplication*,gpointer data) {
        static_cast<RendererProbe*>(data)->activate();
    }),&p);
    const auto result=g_application_run(G_APPLICATION(p.app),1,argv);
    g_object_unref(p.app);p.metrics.sample("finalized",p.frames);
    return result;
}
