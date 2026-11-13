#pragma once
#include <glib.h>
#include <clocale>
#include <cstring>
#include <string>

// Normalize gettext's per-process preference before GTK/libadwaita initialize,
// so their built-in labels share the application's supported-language fallback.
inline void initializeUiLocale() {
    std::setlocale(LC_ALL, "");
    std::string language = "en";
    for (auto languages = g_get_language_names(); *languages; ++languages) {
        const auto* candidate = *languages;
        if ((!std::strncmp(candidate, "ja", 2) || !std::strncmp(candidate, "en", 2))
            && (!candidate[2] || candidate[2] == '_' || candidate[2] == '.' || candidate[2] == '@' || candidate[2] == '-')) {
            language.assign(candidate, 2); break;
        }
    }
    const std::string messages(std::setlocale(LC_MESSAGES, nullptr));
    g_setenv("LANGUAGE", language.c_str(), TRUE);
    // Invalidate gettext's cache without changing the user's locale on disk.
    std::setlocale(LC_MESSAGES, "C"); std::setlocale(LC_MESSAGES, messages.c_str());
}
