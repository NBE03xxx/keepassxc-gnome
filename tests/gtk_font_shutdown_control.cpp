// Diagnostic only: force font shutdown AFTER this standalone GUI exits.
// This sequence must not be copied into the product; it changes global library state.
// Baseline for toolkit/font shutdown allocations. No project session/model,
// clipboard provider, notification watcher, IPC or cryptography is linked.
#include <adwaita.h>
#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangofc-fontmap.h>
PangoFontMap* actualMap = nullptr;
#include <clocale>
#include <cstdio>
int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    auto* app = adw_application_new("org.keepassxc.Gnome.GtkMemoryControl", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](AdwApplication* app, gpointer) {
        auto* window = adw_application_window_new(GTK_APPLICATION(app));
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_box_append(GTK_BOX(box), gtk_label_new("合成 GTK メモリ対照 / Synthetic control"));
        auto* entry = gtk_password_entry_new(); gtk_editable_set_text(GTK_EDITABLE(entry), "synthetic-control-only");
        gtk_box_append(GTK_BOX(box), entry); adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
        gtk_window_set_default_size(GTK_WINDOW(window), 640, 400); gtk_window_present(GTK_WINDOW(window));
        g_timeout_add(1000, +[](gpointer window) -> gboolean {
            actualMap = PANGO_FONT_MAP(g_object_ref(pango_context_get_font_map(gtk_widget_get_pango_context(GTK_WIDGET(window)))));
            auto* app = gtk_window_get_application(GTK_WINDOW(window));
            std::puts("PASS GTK control: standalone toolkit rendering and shutdown"); std::fflush(stdout);
            gtk_window_destroy(GTK_WINDOW(window)); g_application_quit(G_APPLICATION(app)); return G_SOURCE_REMOVE;
        }, window);
    }), nullptr);
    const auto result = g_application_run(G_APPLICATION(app), argc, argv);
    auto* display = gdk_display_get_default(); if (display) g_object_ref(display);
    g_object_unref(app);
    for (unsigned i=0;i<100;++i) { g_main_context_iteration(nullptr,FALSE); g_usleep(1000); }
    if (display) { gdk_display_close(display); g_object_unref(display); }
    for (unsigned i=0;i<100;++i) { g_main_context_iteration(nullptr,FALSE); g_usleep(1000); }
    if (actualMap) { pango_fc_font_map_shutdown(PANGO_FC_FONT_MAP(actualMap)); g_object_unref(actualMap); }
    pango_cairo_font_map_set_default(nullptr);
    FcFini();
    return result;
}
