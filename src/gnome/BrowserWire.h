// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace BrowserWire
{
inline constexpr std::uint32_t MaximumFrame = 64 * 1024;
inline constexpr auto HostName = "org.keepassxc.keepassxc_browser";
inline constexpr auto ExtensionOrigin = "chrome-extension://oboonakemofpalcgghocfoadofidjkkk/";
inline constexpr auto Executable = "keepassxc-gnome-browser-host";
inline constexpr auto SocketName = "browser.sock";

inline bool sameUser(int socket)
{
    struct ucred identity {};
    socklen_t size = sizeof(identity);
    return ::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &identity, &size) == 0
        && size == sizeof(identity) && identity.uid == ::geteuid();
}

inline bool privateDirectory(const std::filesystem::path& path)
{
    struct stat item {};
    return path.is_absolute() && ::lstat(path.c_str(), &item) == 0 && S_ISDIR(item.st_mode)
        && item.st_uid == ::geteuid() && (item.st_mode & 0777) == 0700;
}

inline std::filesystem::path runtimeDirectory()
{
    const auto* root = std::getenv("XDG_RUNTIME_DIR");
    if (!root || !privateDirectory(root)) return {};
    return std::filesystem::path(root) / "keepassxc-gnome-browser";
}

inline bool socketAddress(const std::filesystem::path& dir, sockaddr_un& addr)
{
    const auto path = dir / SocketName;
    if (dir.empty() || path.native().size() >= sizeof(addr.sun_path)) return false;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.native().size() + 1);
    return true;
}

inline QByteArray frame(const QByteArray& body)
{
    if (body.isEmpty() || body.size() > MaximumFrame) return {};
    const auto length = std::uint32_t(body.size());
    QByteArray framed(reinterpret_cast<const char*>(&length), sizeof(length));
    framed += body;
    return framed;
}

class Reader
{
public:
    enum class Result { Await, Complete, Closed };

    Result read(int fd)
    {
        constexpr qsizetype prefixSize = sizeof(std::uint32_t);
        while (true) {
            if (used == prefixSize && body.isEmpty()) {
                std::uint32_t length{};
                std::memcpy(&length, prefix.data(), sizeof(length));
                if (length == 0 || length > MaximumFrame) return Result::Closed;
                body.resize(length);
            }
            const bool header = used < prefixSize;
            const auto size = header ? prefixSize : prefixSize + body.size();
            if (used == size) return Result::Complete;
            auto* destination = header ? prefix.data() + used : body.data() + used - prefixSize;
            const auto n = ::read(fd, destination, size - used);
            if (n > 0) {
                used += n;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return Result::Await;
            } else {
                return Result::Closed;
            }
        }
    }

    bool partial() const { return used != 0; }
    QByteArray take()
    {
        used = 0;
        return std::exchange(body, QByteArray{});
    }

private:
    std::array<char, sizeof(std::uint32_t)> prefix{};
    qsizetype used = 0;
    QByteArray body;
};
}
