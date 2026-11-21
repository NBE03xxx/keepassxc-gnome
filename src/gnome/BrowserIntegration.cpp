// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnome/BrowserIntegration.h"
#include "gnome/BrowserWire.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <glib-unix.h>
#include <limits>
#include <map>
#include <poll.h>
#include <sys/file.h>

namespace fs = std::filesystem;

namespace
{
class Fd
{
public:
    explicit Fd(int socket = -1) : fd(socket) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd; }
    int release() { return std::exchange(fd, -1); }
    void reset(int replacement = -1) { if (fd >= 0) ::close(fd); fd = replacement; }

private:
    int fd;
};

bool sameItem(const struct stat& a, const struct stat& b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

struct Manifest
{
    enum class Entry { Missing, Ours, Foreign, Error };

    fs::path directory;
    fs::path executable;
    QByteArray bytes;
    static constexpr auto Filename = "org.keepassxc.keepassxc_browser.json";
    static constexpr auto System = "/etc/opt/chrome/native-messaging-hosts/org.keepassxc.keepassxc_browser.json";

    Manifest()
    {
        const auto* config = g_getenv("CHROME_CONFIG_HOME");
        fs::path base(config && fs::path(config).is_absolute() ? config : g_get_user_config_dir());
        if (base.is_absolute()) directory = base / "google-chrome" / "NativeMessagingHosts";
        executable = QCoreApplication::applicationDirPath().toStdString();
        executable /= BrowserWire::Executable;
        bytes = QJsonDocument(QJsonObject{
            {"name", BrowserWire::HostName},
            {"description", "KeePassXC GNOME browser integration"},
            {"path", QFile::decodeName(executable.c_str())},
            {"type", "stdio"},
            {"allowed_origins", QJsonArray{BrowserWire::ExtensionOrigin}},
        }).toJson(QJsonDocument::Indented);
    }

    bool systemClear() const
    {
        struct stat item {};
        return ::lstat(System, &item) != 0 && errno == ENOENT;
    }

    int openDirectory() const
    {
        if (directory.empty()) { errno = EINVAL; return -1; }
        Fd fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (fd.get() < 0) return -1;
        struct stat item {};
        if (::fstat(fd.get(), &item) != 0 || item.st_uid != ::geteuid() || (item.st_mode & 0022)) {
            errno = EACCES;
            return -1;
        }
        return fd.release();
    }

    Entry entry(int dir, struct stat* identity = nullptr) const
    {
        struct stat item {};
        if (::fstatat(dir, Filename, &item, AT_SYMLINK_NOFOLLOW) != 0) {
            return errno == ENOENT ? Entry::Missing : Entry::Error;
        }
        if (!S_ISREG(item.st_mode) || item.st_uid != ::geteuid() || item.st_nlink != 1
            || (item.st_mode & 0777) != 0600 || item.st_size != bytes.size()) return Entry::Foreign;

        Fd fd(::openat(dir, Filename, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        if (fd.get() < 0) return Entry::Error;
        struct stat verified {};
        if (::fstat(fd.get(), &verified) != 0 || !sameItem(item, verified)) return Entry::Foreign;
        QByteArray contents(bytes.size(), Qt::Uninitialized);
        if (::pread(fd.get(), contents.data(), contents.size(), 0) != contents.size() || contents != bytes) {
            return Entry::Foreign;
        }
        if (identity) *identity = verified;
        return Entry::Ours;
    }

    Entry entry() const
    {
        Fd dir(openDirectory());
        if (dir.get() < 0) return errno == ENOENT ? Entry::Missing : Entry::Error;
        return entry(dir.get());
    }

    bool publish() const
    {
        if (directory.empty() || g_mkdir_with_parents(directory.c_str(), 0700) != 0) return false;
        Fd dir(openDirectory());
        if (dir.get() < 0 || !systemClear()) return false;
        const auto status = entry(dir.get());
        if (status == Entry::Ours) return true;
        if (status != Entry::Missing) return false;
        auto* uuid = g_uuid_string_random();
        QByteArray name = QByteArrayLiteral(".keepassxc-gnome-") + uuid;
        g_free(uuid);
        Fd fd(::openat(dir.get(), name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (fd.get() < 0) return false;
        const bool published = ::write(fd.get(), bytes.constData(), bytes.size()) == bytes.size() && ::fsync(fd.get()) == 0
            && ::renameat2(dir.get(), name.constData(), dir.get(), Filename, RENAME_NOREPLACE) == 0;
        if (!published) ::unlinkat(dir.get(), name.constData(), 0);
        return published;
    }

    bool remove() const
    {
        Fd dir(openDirectory());
        if (dir.get() < 0) return false;
        struct stat checked {}, current {};
        return entry(dir.get(), &checked) == Entry::Ours
            && ::fstatat(dir.get(), Filename, &current, AT_SYMLINK_NOFOLLOW) == 0
            && sameItem(checked, current) && ::unlinkat(dir.get(), Filename, 0) == 0;
    }
};
}

struct BrowserIntegration::Impl
{
    struct Peer {
        explicit Peer(int fd) : socket(fd) {}
        Fd socket;
        BrowserWire::Reader input;
        QByteArray output;
        qsizetype sent = 0;
        guint watch = 0;
        guint timer = 0;
        bool waiting = false;
    };
    struct Source {
        Impl& impl;
        int id;
    };

    Request request;
    Disconnect disconnect;
    Manifest manifest;
    Fd lock;
    Fd listener;
    struct stat registeredSocket {};
    fs::path socketPath;
    guint listening = 0;
    int next = 0;
    std::map<int, Peer> peers;

    Impl(Request request, Disconnect disconnect) : request(std::move(request)), disconnect(std::move(disconnect)) {}
    ~Impl() { stop(false); }

    bool start()
    {
        if (listener.get() >= 0) return true;
        const auto dir = BrowserWire::runtimeDirectory();
        if (dir.empty() || (::mkdir(dir.c_str(), 0700) && errno != EEXIST) || !BrowserWire::privateDirectory(dir)) {
            return false;
        }
        sockaddr_un address {};
        if (!BrowserWire::socketAddress(dir, address)) return false;
        Fd candidate(::open((dir / "instance.lock").c_str(),
                           O_RDONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600));
        struct stat item {};
        if (candidate.get() < 0 || ::fstat(candidate.get(), &item) != 0 || !S_ISREG(item.st_mode)
            || item.st_uid != ::geteuid() || (item.st_mode & 0777) != 0600
            || ::flock(candidate.get(), LOCK_EX | LOCK_NB) != 0) return false;

        // An acquired process lock excludes another bridge instance. Reclaim
        // only a stale socket in the private directory after checking the owner.
        fs::path path(address.sun_path);
        if (::lstat(path.c_str(), &item) == 0) {
            Fd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
            if (!S_ISSOCK(item.st_mode) || item.st_uid != ::geteuid() || probe.get() < 0
                || ::connect(probe.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0
                || errno != ECONNREFUSED || ::unlink(path.c_str()) != 0) return false;
        } else if (errno != ENOENT) {
            return false;
        }
        Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (fd.get() < 0 || ::bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return false;
        if (::chmod(path.c_str(), 0600) != 0 || ::listen(fd.get(), 4) != 0
            || ::lstat(path.c_str(), &registeredSocket) != 0) {
            ::unlink(path.c_str());
            return false;
        }
        socketPath = std::move(path);
        lock.reset(candidate.release());
        listener.reset(fd.release());
        listening = g_unix_fd_add(listener.get(), GIOCondition(G_IO_IN | G_IO_ERR | G_IO_HUP),
            +[](int, GIOCondition condition, gpointer data) -> gboolean {
                auto& self = *static_cast<Impl*>(data);
                if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)
                    || !self.manifest.systemClear() || self.manifest.entry() != Manifest::Entry::Ours) {
                    self.listening = 0;
                    self.stop();
                    return G_SOURCE_REMOVE;
                }
                self.accept();
                return G_SOURCE_CONTINUE;
            }, this);
        return true;
    }

    void stop(bool notify = true)
    {
        if (listening) g_source_remove(std::exchange(listening, 0));
        listener.reset();
        while (!peers.empty()) close(peers.begin()->first, notify);
        struct stat item {};
        if (!socketPath.empty() && ::lstat(socketPath.c_str(), &item) == 0 && sameItem(item, registeredSocket)) {
            ::unlink(socketPath.c_str());
        }
        socketPath.clear();
        lock.reset();
    }

    void accept()
    {
        // Bound work per dispatch even if a same-user process floods the backlog.
        for (int i = 0; i < 4; ++i) {
            Fd fd(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (fd.get() < 0) return;
            if (peers.size() >= 4 || !BrowserWire::sameUser(fd.get()) || next == std::numeric_limits<int>::max()) {
                continue;
            }
            const auto id = ++next;
            peers.try_emplace(id, fd.release());
            arm(id);
        }
    }

    void close(int id, bool notify = true)
    {
        const auto item = peers.find(id);
        if (item == peers.end()) return;
        if (item->second.watch) g_source_remove(item->second.watch);
        if (item->second.timer) g_source_remove(item->second.timer);
        peers.erase(item);
        if (notify) disconnect(id);
    }

    void deadline(int id, unsigned seconds)
    {
        auto& peer = peers.at(id);
        if (peer.timer) g_source_remove(peer.timer);
        peer.timer = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, seconds, +[](gpointer data) -> gboolean {
            auto& source = *static_cast<Source*>(data);
            source.impl.peers.at(source.id).timer = 0;
            source.impl.close(source.id);
            return G_SOURCE_REMOVE;
        }, new Source{*this, id}, +[](gpointer data) { delete static_cast<Source*>(data); });
    }

    void arm(int id)
    {
        auto& peer = peers.at(id);
        if (peer.watch) return;
        auto events = G_IO_HUP | G_IO_ERR | G_IO_NVAL;
        if (!peer.output.isEmpty()) events |= G_IO_OUT;
        else if (!peer.waiting) events |= G_IO_IN;
        peer.watch = g_unix_fd_add_full(G_PRIORITY_DEFAULT, peer.socket.get(), GIOCondition(events),
            +[](int, GIOCondition condition, gpointer data) -> gboolean {
                auto& source = *static_cast<Source*>(data);
                source.impl.peers.at(source.id).watch = 0;
                source.impl.ready(source.id, condition);
                return G_SOURCE_REMOVE;
            }, new Source{*this, id}, +[](gpointer data) { delete static_cast<Source*>(data); });
    }

    void ready(int id, GIOCondition condition)
    {
        if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) { close(id); return; }
        auto& peer = peers.at(id);
        if (condition & G_IO_OUT) {
            const auto count = ::send(peer.socket.get(), peer.output.constData() + peer.sent,
                                      peer.output.size() - peer.sent, MSG_NOSIGNAL);
            if (count > 0) {
                peer.sent += count;
            } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                close(id);
                return;
            }
            if (peer.sent == peer.output.size()) {
                peer.sent = 0;
                peer.output.clear();
                g_source_remove(std::exchange(peer.timer, 0));
            }
        } else if (condition & G_IO_IN) {
            const auto result = peer.input.read(peer.socket.get());
            if (result == BrowserWire::Reader::Result::Closed) { close(id); return; }
            if (result == BrowserWire::Reader::Result::Complete) {
                const auto data = peer.input.take();
                const auto document = QJsonDocument::fromJson(data);
                if (!data.isValidUtf8() || !document.isObject()) { close(id); return; }
                peer.waiting = true;
                deadline(id, 65);
                request(id, document.object());
            } else if (peer.input.partial() && !peer.timer) {
                deadline(id, 5);
            }
        }
        if (peers.contains(id)) arm(id);
    }
};

BrowserIntegration::BrowserIntegration(Request request, Disconnect disconnect)
    : impl(std::make_unique<Impl>(std::move(request), std::move(disconnect)))
{
    if (impl->manifest.systemClear() && impl->manifest.entry() == Manifest::Entry::Ours
        && ::access(impl->manifest.executable.c_str(), X_OK) == 0) impl->start();
}

BrowserIntegration::~BrowserIntegration() = default;

BrowserIntegration::State BrowserIntegration::state() const
{
    if (!impl->manifest.systemClear()) return State::Conflict;
    switch (impl->manifest.entry()) {
    case Manifest::Entry::Foreign: return State::Conflict;
    case Manifest::Entry::Error: return State::Unavailable;
    case Manifest::Entry::Ours: return impl->listener.get() >= 0 ? State::Enabled : State::Unavailable;
    case Manifest::Entry::Missing: return State::Disabled;
    }
    return State::Unavailable;
}

BrowserIntegration::State BrowserIntegration::enable()
{
    const auto before = state();
    if (before == State::Conflict || before == State::Enabled) return before;
    if (::access(impl->manifest.executable.c_str(), X_OK) == 0 && impl->start()) {
        if (impl->manifest.publish()) return State::Enabled;
        impl->stop();
        if (state() == State::Conflict) return State::Conflict;
    }
    return State::Unavailable;
}

BrowserIntegration::State BrowserIntegration::disable()
{
    const bool active = impl->listener.get() >= 0;
    const bool removed = active && impl->manifest.remove();
    impl->stop();
    const auto after = state();
    if (removed || after == State::Conflict || after == State::Disabled) return after;
    return State::Unavailable;
}

bool BrowserIntegration::connected(int connection) const
{
    const auto item = impl->peers.find(connection);
    if (item == impl->peers.end()) return false;
    pollfd fd {item->second.socket.get(), 0, 0};
    return ::poll(&fd, 1, 0) == 0;
}

void BrowserIntegration::send(int connection, const QJsonObject& response)
{
    const auto item = impl->peers.find(connection);
    if (item == impl->peers.end() || !item->second.waiting) return;
    auto& peer = item->second;
    if (response.isEmpty() || !connected(connection)) { impl->close(connection); return; }
    peer.output = BrowserWire::frame(QJsonDocument(response).toJson(QJsonDocument::Compact));
    if (peer.output.isEmpty()) { impl->close(connection); return; }
    peer.waiting = false;
    impl->deadline(connection, 5);
    if (peer.watch) g_source_remove(std::exchange(peer.watch, 0));
    impl->arm(connection);
}

void BrowserIntegration::closeConnections()
{
    while (!impl->peers.empty()) impl->close(impl->peers.begin()->first);
}
