// Baseline for toolkit/font shutdown allocations. No project session/model,
// clipboard provider, notification watcher, IPC or cryptography is linked.
#include <adwaita.h>
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
            auto* app = gtk_window_get_application(GTK_WINDOW(window));
            std::puts("PASS GTK control: standalone toolkit rendering and shutdown"); std::fflush(stdout);
            gtk_window_destroy(GTK_WINDOW(window)); g_application_quit(G_APPLICATION(app)); return G_SOURCE_REMOVE;
        }, window);
    }), nullptr);
    const auto result = g_application_run(G_APPLICATION(app), argc, argv); g_object_unref(app); return result;
}
