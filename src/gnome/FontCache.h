#pragma once

#include <gtk/gtk.h>
#include <pango/pangofc-fontmap.h>

// Per-window maintenance on the GTK thread. Drop Pango's retained scaled-font
// patterns through its public API; live layouts/fonts retain their references.
// Do not shut down the font map, FcConfig, display, or rendering backend.
class FontCache
{
public:
    static constexpr gint64 Interval = 5 * 60 * G_USEC_PER_SEC;

    explicit FontCache(GtkWidget* root) : thread(g_thread_self()), last(g_get_monotonic_time()) {
        g_weak_ref_init(&widget, root);
    }
    ~FontCache() { g_weak_ref_clear(&widget); }
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;

    static bool clear(PangoFontMap* map) {
        if (!map || !PANGO_IS_FC_FONT_MAP(map)) return false;
        // cache_clear emits notifications; keep the map alive throughout them.
        g_object_ref(map);
        pango_fc_font_map_cache_clear(PANGO_FC_FONT_MAP(map));
        g_object_unref(map);
        return true;
    }

    bool maintain(gint64 now = g_get_monotonic_time()) {
        g_return_val_if_fail(g_thread_self() == thread, false);
        if (now <= last || now - last < Interval) return false;
        auto* root = GTK_WIDGET(g_weak_ref_get(&widget));
        if (!root) return false;
        bool cleared = false;
        if (gtk_widget_get_realized(root) && !gtk_widget_in_destruction(root)) {
            auto* context = gtk_widget_get_pango_context(root);
            auto* map = pango_context_get_font_map(context);
            last = now; // Notifications cannot re-enter a due maintenance call.
            const auto started = g_get_monotonic_time();
            cleared = clear(map);
            if (cleared) {
                ++clears;
                const auto elapsed = g_get_monotonic_time() - started;
                if (elapsed > longest) longest = elapsed;
            }
        }
        g_object_unref(root);
        return cleared;
    }

    unsigned clearCount() const { return clears; }
    gint64 longestClearUs() const { return longest; }

private:
    GWeakRef widget;
    GThread* thread;
    gint64 last;
    unsigned clears = 0;
    gint64 longest = 0;
};
