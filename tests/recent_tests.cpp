#include "gnome/RecentDatabase.h"
#include <filesystem>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool ok, const char* text) { if (!ok) { std::fprintf(stderr, "FAIL recent: %s\n", text); std::exit(1); } ++checks; }
}
int main() {
    auto* temporary = g_dir_make_tmp("kpxc-recent-XXXXXX", nullptr); check(temporary, "private test folder");
    const std::string root(temporary); g_free(temporary);
    const auto directory = root + "/private";
    RecentDatabase a(directory);
    check(a.path().empty(), "first use has no history");
    const std::string first = "/unmounted/synthetic/日本語 & <mail>.kdbx";
    check(a.remember(first), "remember metadata without opening the unavailable DB");
    RecentDatabase b(directory); check(b.path() == first, "restart preserves exactly one UTF-8 path");
    struct stat s{}; check(!stat((directory + "/recent.ini").c_str(), &s) && (s.st_mode & 0777) == 0600, "metadata private mode");
    check(!stat(directory.c_str(), &s) && (s.st_mode & 0777) == 0700, "directory private mode");
    check(!a.remember("relative.kdbx") && !a.remember(std::string("/x\0hidden", 9)) && !a.remember("/" + std::string(4097, 'x')), "invalid paths never recorded");
    check(RecentDatabase(directory).path() == first, "rejection preserves prior record");
    check(a.remember("/tmp/second.kdbx") && RecentDatabase(directory).path() == "/tmp/second.kdbx", "later success replaces previous record");
    check(b.forget() && RecentDatabase(directory).path().empty(), "forget removes record across instances");
    check(a.remember("/tmp/third.kdbx"), "record after forget is a fresh recent, not a fixed default");
    int fd = open((directory + "/recent.lock").c_str(), O_RDWR | O_CLOEXEC); check(fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB), "hold competing metadata lock");
    check(!a.remember(first), "competing process never blocks GTK or loses an update"); close(fd);
    check(RecentDatabase(directory).path() == "/tmp/third.kdbx", "busy metadata unchanged");
    const auto metadata = directory + "/recent.ini", outside = root + "/outside";
    check(!rename(metadata.c_str(), outside.c_str()) && !symlink(outside.c_str(), metadata.c_str()), "symlink fixture");
    check(RecentDatabase(directory).path().empty() && !a.forget(), "symlink is neither read nor overwritten");
    check(std::filesystem::is_symlink(metadata), "foreign symlink unchanged"); unlink(metadata.c_str());
    check(!link(outside.c_str(), metadata.c_str()), "hardlink fixture");
    check(RecentDatabase(directory).path().empty() && !a.remember(first), "hardlink is neither read nor replaced"); unlink(metadata.c_str());
    check(g_file_set_contents(metadata.c_str(), "[Recent]\nPath=/tmp/exposed.kdbx\n", -1, nullptr), "permission fixture"); chmod(metadata.c_str(), 0644);
    check(RecentDatabase(directory).path().empty() && !a.remember(first), "exposed permissions refused"); chmod(metadata.c_str(), 0600);
    const auto oversize = std::string(20000, 'x'); check(g_file_set_contents(metadata.c_str(), oversize.data(), oversize.size(), nullptr), "oversize fixture");
    check(RecentDatabase(directory).path().empty() && !a.remember(first), "oversize data bounded and not overwritten");
    check(g_file_set_contents(metadata.c_str(), "garbled", -1, nullptr), "malformed fixture");
    check(RecentDatabase(directory).path().empty() && !a.forget(), "malformed metadata safely ignored");
    unlink(metadata.c_str()); check(a.remember(first) && a.forget(), "clean metadata remains usable");
    std::filesystem::remove_all(root);
    std::printf("PASS recent metadata: %u checks\n", checks);
}
