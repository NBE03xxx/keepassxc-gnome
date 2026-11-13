#pragma once
#include <adwaita.h>

// Use libadwaita's platform preference; on GNOME, fall back to its read-only
// desktop setting when dump protection prevents portal identification.
class Appearance {
public:
    explicit Appearance(AdwStyleManager* manager) : manager(manager) {
        g_object_ref(manager);
        auto* source = g_settings_schema_source_get_default();
        auto* schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : nullptr;
        if (schema) {
            if (g_settings_schema_has_key(schema, "color-scheme")) settings = g_settings_new_full(schema, nullptr, nullptr);
            g_settings_schema_unref(schema);
        }
        if (settings) g_signal_connect(settings, "changed::color-scheme", G_CALLBACK(+[](GSettings*, const char*, gpointer p) { static_cast<Appearance*>(p)->apply(); }), this);
        g_signal_connect(manager, "notify::system-supports-color-schemes", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer p) { static_cast<Appearance*>(p)->apply(); }), this);
        apply();
    }
    ~Appearance() { g_signal_handlers_disconnect_by_data(manager, this); g_clear_object(&settings); g_object_unref(manager); }
    void apply() {
        auto scheme = ADW_COLOR_SCHEME_DEFAULT;
        if (settings && !adw_style_manager_get_system_supports_color_schemes(manager)) {
            auto* value = g_settings_get_string(settings, "color-scheme");
            if (g_str_equal(value, "prefer-dark")) scheme = ADW_COLOR_SCHEME_FORCE_DARK;
            else if (g_str_equal(value, "prefer-light")) scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
            g_free(value);
        }
        adw_style_manager_set_color_scheme(manager, scheme);
    }
private:
    AdwStyleManager* manager;
    GSettings* settings = nullptr;
};
