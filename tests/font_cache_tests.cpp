// A retained layout and font must survive repeated application cache trims.
#include "gnome/FontCache.h"
#include <pango/pangocairo.h>
#include <cstdio>
#include <cstdlib>
namespace {
unsigned checks = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL font cache: %s\n", message); std::exit(2); }
    ++checks;
}
}
int main() {
    check(!FontCache::clear(nullptr), "null map is harmless");
    auto* map = pango_cairo_font_map_new();
    pango_cairo_font_map_set_resolution(PANGO_CAIRO_FONT_MAP(map), 132.0);
    auto* context = pango_font_map_create_context(map);
    auto* description = pango_font_description_from_string("Sans 12");
    auto* font = pango_font_map_load_font(map, context, description);
    check(font, "base font available");
    auto* layout = pango_layout_new(context);
    pango_layout_set_font_description(layout, description);
    pango_layout_set_text(layout, "日本語 English / synthetic text", -1);
    int width = 0, height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    check(width > 0 && height > 0, "initial layout shaped");
    for (unsigned batch = 0; batch < 8; ++batch) {
        for (unsigned n = 0; n < 500; ++n) {
            auto* scaled = pango_font_map_reload_font(map, font, .8 + double(n + batch)/2000, context, nullptr);
            check(scaled, "scaled font available"); g_object_unref(scaled);
        }
        const auto serial = pango_font_map_get_serial(map);
        check(FontCache::clear(map), "application helper trims the supported map");
        check(pango_cairo_font_map_get_resolution(PANGO_CAIRO_FONT_MAP(map)) == 132.0,
              "configured font resolution survives maintenance");
        check(pango_font_map_get_serial(map) != serial, "live contexts see cache invalidation");
        int afterWidth = 0, afterHeight = 0;
        pango_layout_get_pixel_size(layout, &afterWidth, &afterHeight);
        check(afterWidth == width && afterHeight == height, "held Japanese/English layout keeps its extents");
        auto* metrics = pango_font_get_metrics(font, nullptr);
        check(metrics && pango_font_metrics_get_ascent(metrics) > 0, "held font remains usable");
        if (metrics) pango_font_metrics_unref(metrics);
    }
    g_object_unref(layout); g_object_unref(font); pango_font_description_free(description);
    g_object_unref(context); g_object_unref(map);
    std::printf("PASS font cache: %u checks, 8 trims, 4000 scaled fonts, retained bilingual layout\n", checks);
}
