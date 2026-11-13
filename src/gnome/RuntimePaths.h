#pragma once
#include <filesystem>
// Both a build tree (bin/../share) and a staged/installed private libexec
// directory resolve without environment overrides or the working directory.
inline std::filesystem::path localeDirectory() {
    const auto directory = std::filesystem::canonical("/proc/self/exe").parent_path();
    if (directory.filename() == "keepassxc-gnome" && directory.parent_path().filename() == "libexec")
        return directory.parent_path().parent_path() / "share/locale";
    return directory.parent_path() / "share/locale";
}
