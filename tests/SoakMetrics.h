#pragma once
// Synthetic GUI soak only. Metadata, never memory contents or clipboard text.
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <cstdint>
#ifdef __SANITIZE_ADDRESS__
extern "C" size_t __sanitizer_get_current_allocated_bytes();
extern "C" size_t __sanitizer_get_heap_size();
#endif
struct SoakMetrics {
    gint64 start = g_get_monotonic_time(), last = 0;
    std::string path;
    unsigned fontCacheClears = 0;
    gint64 longestFontClearUs = 0;
    explicit SoakMetrics(const std::string& root) : path(root + "/memory.jsonl") {
        std::ofstream reset(path);
    }
    static unsigned count(const char* path) {
        unsigned n = 0;
        if (auto* dir = opendir(path)) {
            while (auto* item = readdir(dir)) if (item->d_name[0] != '.') ++n;
            closedir(dir);
        }
        return n;
    }
    void sample(const char* phase, unsigned cycle, bool force = true) {
        const auto now = g_get_monotonic_time();
        if (!force && now - last < 30000000) return;
        last = now;
        uint64_t rss = 0, anonymous = 0, size = 0;
        std::ifstream input("/proc/self/status"); std::string line;
        while (std::getline(input, line)) {
            std::istringstream row(line); std::string key; uint64_t value = 0;
            row >> key >> value;
            if (key == "VmRSS:") rss = value;
            if (key == "RssAnon:") anonymous = value;
            if (key == "VmSize:") size = value;
        }
        uint64_t live = 0, heap = 0;
#ifdef __SANITIZE_ADDRESS__
        live = __sanitizer_get_current_allocated_bytes();
        heap = __sanitizer_get_heap_size();
#endif
        const auto descriptors = count("/proc/self/fd"), threads = count("/proc/self/task");
        bool mesaMapped = false; std::ifstream maps("/proc/self/maps");
        while (std::getline(maps, line)) if (line.find("libvulkan_radeon.so") != std::string::npos) mesaMapped = true;
        std::ofstream output(path, std::ios::app);
        output << "{\"elapsed_s\":" << double(now-start)/1000000
               << ",\"phase\":\"" << phase << "\",\"cycle\":" << cycle
               << ",\"rss_kib\":" << rss << ",\"anonymous_kib\":" << anonymous
               << ",\"virtual_kib\":" << size << ",\"live_bytes\":" << live
               << ",\"heap_bytes\":" << heap << ",\"fds\":" << descriptors
               << ",\"threads\":" << threads << ",\"mesa_mapped\":" << int(mesaMapped)
               << ",\"font_cache_clears\":" << fontCacheClears
               << ",\"longest_font_clear_us\":" << longestFontClearUs << "}\n";
    }
};
