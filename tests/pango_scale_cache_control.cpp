// Synthetic cache-retention contrast. No GTK, Qt, IPC, DB or clipboard.
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <cstdio>
#include <cstdlib>
extern "C" size_t __sanitizer_get_current_allocated_bytes();
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    auto* map=pango_cairo_font_map_new();auto* context=pango_font_map_create_context(map);
    auto* description=pango_font_description_from_string("Sans 12");
    auto* font=pango_font_map_load_font(map,context,description);
    if(!font)return 2;
    auto reload=[&](double scale) {
        auto* scaled=pango_font_map_reload_font(map,font,scale,context,nullptr);
        if(!scaled)std::abort();
        g_object_unref(scaled);
    };
    for(unsigned n=0;n<32;++n)reload(1.05);
    const auto before=__sanitizer_get_current_allocated_bytes();
    for(unsigned n=0;n<1000;++n)reload(1.05);
    const auto same=__sanitizer_get_current_allocated_bytes();
    for(unsigned n=0;n<1000;++n)reload(.8+double(n)/2000);
    const auto distinct=__sanitizer_get_current_allocated_bytes();
    // Direct contrast only; the product/soak uses scheduled five-minute trims.
    pango_fc_font_map_cache_clear(PANGO_FC_FONT_MAP(map));
    const auto cleared=__sanitizer_get_current_allocated_bytes();
    auto* metrics=pango_font_get_metrics(font,nullptr);
    const bool valid=metrics && pango_font_metrics_get_ascent(metrics)>0;
    if(metrics)pango_font_metrics_unref(metrics);
    std::printf("SCALE CACHE: before=%zu same_1000=%zu distinct_1000=%zu after_public_clear=%zu base_font_valid=%d\n",before,same,distinct,cleared,valid);
    const bool contrast=distinct>same+256*1024 && cleared<distinct && valid;
    pango_font_description_free(description);g_object_unref(font);g_object_unref(context);g_object_unref(map);
    std::printf("%s Pango scale contrast; final LSan remains an independent result\n",contrast?"PASS":"FAIL");
    return contrast?0:3;
}
