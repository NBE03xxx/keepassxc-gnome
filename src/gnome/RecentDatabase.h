#pragma once

#include <glib.h>
#include <cerrno>
#include <cstdio>
#include <utility>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// One local filename, never a key or database content. All operations use a
// bounded private metadata file, not the database itself. No startup stat/open
// of the remembered database (it may be on removable or unavailable storage).
class RecentDatabase
{
public:
    explicit RecentDatabase(std::string directory = std::string(g_get_user_state_dir()) + "/keepassxc-gnome")
        : directory(std::move(directory)) { transaction([] { return false; }); }
    const std::string& path() const { return filename; }
    bool remember(const std::string& path) {
        if (!validPath(path)) return false;
        return transaction([&] { filename = path; return true; });
    }
    bool forget() { return transaction([&] { filename.clear(); return true; }); }


private:
    static constexpr size_t Limit = 16384;
    static bool validPath(const std::string& path) {
        return !path.empty() && path.front() == '/' && path.size() <= 4096 &&
            path.find('\0') == std::string::npos && g_utf8_validate(path.data(), path.size(), nullptr);
    }
    static bool privateFile(int fd) {
        struct stat s{};
        return fd >= 0 && !fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_uid == geteuid() &&
            !(s.st_mode & 0077) && s.st_nlink == 1 && s.st_size <= static_cast<off_t>(Limit);
    }
    int openDirectory() const {
        if (g_mkdir_with_parents(directory.c_str(), 0700)) return -1;
        const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat s{};
        if (fd >= 0 && !fstat(fd, &s) && s.st_uid == geteuid() && !(s.st_mode & 0077)) return fd;
        if (fd >= 0) close(fd);
        return -1;
    }
    bool read(int dir) {
        const int fd = openat(dir, "recent.ini", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) { if (errno != ENOENT) return false; filename.clear(); return true; }
        if (!privateFile(fd)) { close(fd); return false; }
        std::string bytes(Limit + 1, '\0'); size_t count = 0;
        while (count < bytes.size()) {
            const auto n = ::read(fd, bytes.data() + count, bytes.size() - count);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { close(fd); return false; }
            if (!n) break;
            count += static_cast<size_t>(n);
        }
        close(fd);
        if (!count || count > Limit || bytes.find('\0', 0) < count) return false;
        auto* data = g_key_file_new(); GError* error = nullptr;
        bool ok = g_key_file_load_from_data(data, bytes.data(), count, G_KEY_FILE_NONE, &error);
        std::string path;
        if (ok && g_key_file_has_key(data, "Recent", "Path", nullptr)) {
            auto* value = g_key_file_get_string(data, "Recent", "Path", &error);
            if (value) { path = value; g_free(value); }
            ok = !error && (path.empty() || validPath(path));
        }
        g_clear_error(&error); g_key_file_unref(data);
        if (ok) filename = path;
        return ok;
    }
    bool write(int dir) const {
        auto* data = g_key_file_new();
        g_key_file_set_string(data, "Recent", "Path", filename.c_str());
        gsize size = 0; auto* bytes = g_key_file_to_data(data, &size, nullptr); g_key_file_unref(data);
        auto* uuid = g_uuid_string_random(); const auto temporary = std::string(".recent-") + uuid; g_free(uuid);
        const int fd = openat(dir, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        bool ok = fd >= 0; size_t count = 0;
        while (ok && count < size) {
            const auto n = ::write(fd, bytes + count, size - count);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) ok = false;
            else count += static_cast<size_t>(n);
        }
        g_free(bytes);
        if (fd >= 0) { if (ok) ok = !fsync(fd); if (close(fd)) ok = false; }
        if (ok) ok = !renameat(dir, temporary.c_str(), dir, "recent.ini");
        if (ok) ok = !fsync(dir);
        if (!ok) unlinkat(dir, temporary.c_str(), 0);
        return ok;
    }
    template<typename Change> bool transaction(Change change) {
        const int dir = openDirectory(); if (dir < 0) return false;
        const int lock = openat(dir, "recent.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        bool ok = privateFile(lock) && !flock(lock, LOCK_EX | LOCK_NB);
        if (ok) {
            ok = read(dir);
            if (!ok) filename.clear();
            else if (change() && !write(dir)) {
                // A failed fsync may still have published the new metadata.
                // Re-read while locked; never claim an uncommitted selection.
                ok = false; filename.clear(); read(dir);
            }
        }
        if (lock >= 0) close(lock);
        close(dir); return ok;
    }
    std::string directory, filename;
};
