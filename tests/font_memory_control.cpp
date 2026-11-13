// Diagnostic only: independent font-library ownership, no application code.
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <cstdio>
int main(int argc,char**){
 FcInit();
 if(argc>1){
  auto* map=pango_cairo_font_map_new();
  auto* context=pango_font_map_create_context(map);
  auto* layout=pango_layout_new(context);
  pango_layout_set_text(layout,"日本語の合成文字列 / synthetic control",-1);
  int width,height;pango_layout_get_pixel_size(layout,&width,&height);
  g_object_unref(layout);g_object_unref(context);
  pango_fc_font_map_shutdown(PANGO_FC_FONT_MAP(map));g_object_unref(map);
 }else{
  auto* pattern=FcPatternCreate();auto* objects=FcObjectSetBuild(FC_FAMILY,FC_STYLE,nullptr);
  auto* fonts=FcFontList(nullptr,pattern,objects);
  FcFontSetDestroy(fonts);FcObjectSetDestroy(objects);FcPatternDestroy(pattern);
 }
 FcFini();std::puts("PASS font-only control");
}
