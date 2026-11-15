#pragma once
#include <adwaita.h>
#include <string>

// Prefer the platform's libadwaita settings. On GNOME, read the desktop
// preferences if the portal cannot identify this dump-protected process.
// CSS is limited to this application's windows; never write desktop settings.
class Appearance {
public:
    Appearance(AdwStyleManager* manager, GtkWidget* root) : manager(manager), display(gtk_widget_get_display(root)) {
        g_object_ref(manager); g_object_ref(display);
        gtk_widget_add_css_class(root, "kpxc-gnome");
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        auto* source = g_settings_schema_source_get_default();
        auto* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
        if (schema) {
            hasScheme = g_settings_schema_has_key(schema, "color-scheme");
            hasAccent = g_settings_schema_has_key(schema, "accent-color");
            if (hasScheme || hasAccent) settings = g_settings_new_full(schema, nullptr, nullptr);
            g_settings_schema_unref(schema);
        }
        if (settings) {
            if (hasScheme) g_signal_connect(settings, "changed::color-scheme", G_CALLBACK(+[](GSettings*, const char*, gpointer p) { static_cast<Appearance*>(p)->apply(); }), this);
            if (hasAccent) g_signal_connect(settings, "changed::accent-color", G_CALLBACK(+[](GSettings*, const char*, gpointer p) { static_cast<Appearance*>(p)->applyAccent(); }), this);
        }
        g_signal_connect(manager, "notify::system-supports-color-schemes", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer p) { static_cast<Appearance*>(p)->apply(); }), this);
        for (const auto* signal : {"notify::system-supports-accent-colors", "notify::dark", "notify::high-contrast"})
            g_signal_connect(manager, signal, G_CALLBACK(+[](GObject*, GParamSpec*, gpointer p) { static_cast<Appearance*>(p)->applyAccent(); }), this);
        apply();
    }
    ~Appearance() {
        g_signal_handlers_disconnect_by_data(manager, this); g_clear_object(&settings);
        gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(provider));
        g_object_unref(provider); g_object_unref(display); g_object_unref(manager);
    }
    void apply() {
        auto scheme = ADW_COLOR_SCHEME_DEFAULT;
        if (settings && hasScheme && !adw_style_manager_get_system_supports_color_schemes(manager)) {
            auto* value = g_settings_get_string(settings, "color-scheme");
            if (g_str_equal(value, "prefer-dark")) scheme = ADW_COLOR_SCHEME_FORCE_DARK;
            else if (g_str_equal(value, "prefer-light")) scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
            g_free(value);
        }
        adw_style_manager_set_color_scheme(manager, scheme);
        applyAccent();
    }
private:
    void applyAccent() {
        // rem follows GTK's configured font and accessibility text scale.
        // Keep this outside the palette override so high contrast stays readable.
        std::string css =
            "window.kpxc-gnome { font-size: 1.2rem; }\n"
            "window.kpxc-gnome .subtitle { font-size: 1em; }\n";
        // Keep the toolkit's own high-contrast rules, outlines and palette.
        if (!adw_style_manager_get_high_contrast(manager)) {
            if (settings && hasAccent && !adw_style_manager_get_system_supports_accent_colors(manager)) {
                auto* value = g_settings_get_string(settings, "accent-color");
                auto* colors = G_ENUM_CLASS(g_type_class_ref(ADW_TYPE_ACCENT_COLOR));
                const auto* found = g_enum_get_value_by_nick(colors, value);
                const auto accent = found ? static_cast<AdwAccentColor>(found->value) : ADW_ACCENT_COLOR_BLUE;
                GdkRGBA background{}, foreground{};
                adw_accent_color_to_rgba(accent, &background);
                adw_accent_color_to_standalone_rgba(accent, adw_style_manager_get_dark(manager), &foreground);
                auto* bg = gdk_rgba_to_string(&background); auto* fg = gdk_rgba_to_string(&foreground);
                css += std::string("window.kpxc-gnome { --accent-bg-color: ") + bg +
                    "; --accent-fg-color: white; --accent-color: " + fg + "; }\n";
                g_free(bg); g_free(fg); g_free(value); g_type_class_unref(colors);
            }
            css += "window.kpxc-gnome listview.kpxc-entry-list > row:selected { background-color: color-mix(in srgb, var(--accent-bg-color) 20%, var(--view-bg-color)); color: var(--view-fg-color); }\n"
                   "window.kpxc-gnome listview.kpxc-entry-list > row:selected .dim-label { opacity: 1; }\n";
        }
        if (css == loadedCss) return;
        loadedCss = css;
        gtk_css_provider_load_from_string(provider, css.c_str());
    }
    AdwStyleManager* manager;
    GdkDisplay* display;
    GtkCssProvider* provider;
    GSettings* settings = nullptr;
    bool hasScheme = false, hasAccent = false;
    std::string loadedCss;
};
