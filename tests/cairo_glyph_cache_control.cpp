// Synthetic cache saturation only: no GTK, Qt, DB, clipboard or static reset.
#include <cairo.h>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
extern "C" size_t __sanitizer_get_current_allocated_bytes();
extern "C" void __sanitizer_print_memory_profile(size_t,size_t);

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    auto* image=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1024,48);
    auto* face=cairo_toy_font_face_create("sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    const char* text="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 !#$%&()*+,-./:;<=>?@[]^_{|}~";
    for(unsigned n=1;n<=4096;++n) {
        auto* cr=cairo_create(image);
        cairo_set_font_face(cr,face);
        // Unique matrix keys, almost equal raster sizes: exercise retention,
        // not ever-larger glyph images.
        cairo_set_font_size(cr,12.0+double(n)/100000.0);
        cairo_move_to(cr,0,24);cairo_show_text(cr,text);
        const auto status=cairo_status(cr);cairo_destroy(cr);
        if(status!=CAIRO_STATUS_SUCCESS)return 2;
        if(n==64 || n==128 || n==256 || n==512 || n==1024 || n==2048 || n==4096) {
            std::printf("CAIRO CACHE fonts=%u live_bytes=%zu\n",n,__sanitizer_get_current_allocated_bytes());
            std::fprintf(stderr,"ALLOCATION PROFILE fonts=%u phase=released-contexts\n",n);std::fflush(stderr);
            __sanitizer_print_memory_profile(100,100);
        }
    }
    cairo_font_face_destroy(face);cairo_surface_destroy(image);
    std::printf("PASS Cairo 4096 matrix variants; raw final LSan is independent\n");
}
