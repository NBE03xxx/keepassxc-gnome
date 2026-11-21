// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnome/BrowserWire.h"

#include <chrono>
#include <poll.h>
#include <signal.h>
#include <string_view>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

bool waitFor(int fd, short events, int watch, Clock::time_point end)
{
    for (;;) {
        int milliseconds = -1;
        if (end != Clock::time_point::max()) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now()).count();
            if (remaining <= 0) return false;
            milliseconds = int(remaining);
        }
        pollfd fds[] = {{fd, events, 0}, {watch, 0, 0}};
        const int count = ::poll(fds, 2, milliseconds);
        if (count < 0 && errno == EINTR) continue;
        return count > 0 && !fds[1].revents && (fds[0].revents & events);
    }
}

bool receive(int fd, int watch, QByteArray& frame, Clock::time_point deadline)
{
    BrowserWire::Reader reader;
    while (waitFor(fd, POLLIN, watch, deadline)) {
        if (deadline == Clock::time_point::max()) deadline = Clock::now() + 5s;
        const auto result = reader.read(fd);
        if (result == BrowserWire::Reader::Result::Closed) return false;
        if (result == BrowserWire::Reader::Result::Complete) {
            frame = BrowserWire::frame(reader.take());
            return true;
        }
    }
    return false;
}

bool forward(int fd, int watch, const QByteArray& frame)
{
    const auto deadline = Clock::now() + 5s;
    qsizetype sent = 0;
    while (sent < frame.size() && waitFor(fd, POLLOUT, watch, deadline)) {
        const auto count = ::write(fd, frame.constData() + sent, frame.size() - sent);
        if (count > 0) sent += count;
        else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
    }
    return sent == frame.size();
}

bool nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
}

int main(int argc, char* argv[])
{
    if (argc != 2 || std::string_view(argv[1]) != BrowserWire::ExtensionOrigin || ::geteuid() == 0) return 1;
    const auto dir = BrowserWire::runtimeDirectory();
    struct stat item {};
    sockaddr_un address {};
    if (!BrowserWire::privateDirectory(dir) || !BrowserWire::socketAddress(dir, address)
        || ::lstat(address.sun_path, &item) != 0 || !S_ISSOCK(item.st_mode)
        || item.st_uid != ::geteuid() || (item.st_mode & 0777) != 0600) return 1;

    ::signal(SIGPIPE, SIG_IGN);
    const auto socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket < 0) return 1;
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || !BrowserWire::sameUser(socket) || !nonblocking(STDIN_FILENO) || !nonblocking(STDOUT_FILENO)) {
        ::close(socket);
        return 1;
    }

    QByteArray frame;
    while (receive(STDIN_FILENO, socket, frame, Clock::time_point::max())
           && forward(socket, STDIN_FILENO, frame)
           && receive(socket, STDIN_FILENO, frame, Clock::now() + 70s)
           && forward(STDOUT_FILENO, socket, frame)) {}
    ::close(socket);
    return 0;
}
