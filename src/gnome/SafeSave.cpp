#include "gnome/SafeSave.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace SafeSave {
namespace {
constexpr qint64 MaximumBytes = 128LL << 20;
constexpr unsigned MaximumRecords = 64;
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
bool regular(int fd, bool privateMode = false)
{
    struct stat s{};
    return fd >= 0 && !::fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_uid == ::geteuid() && s.st_nlink == 1
        && (!privateMode || (s.st_mode & 0777) == 0600);
}
bool safeDirectory(int fd)
{
    struct stat s{};
    return fd >= 0 && !::fstat(fd, &s) && S_ISDIR(s.st_mode) && s.st_uid == ::geteuid() && (s.st_mode & 0777) == 0700;
}
bool bytes(int fd, const QByteArray& data)
{
    qsizetype offset = 0;
    while (offset < data.size()) {
        const auto n = ::write(fd, data.constData() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += n;
    }
    return true;
}
QString actualPath(int fd)
{
    char path[8192];
    const auto link = QByteArray("/proc/self/fd/") + QByteArray::number(fd);
    const auto size = ::readlink(link.constData(), path, sizeof(path));
    if (size <= 0 || size == sizeof(path)) return {};
    return QFile::decodeName(QByteArray(path, size));
}
bool directoryStillThere(int fd, const QString& path)
{
    Fd current(::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    struct stat a{}, b{};
    return current.value >= 0 && !::fstat(fd, &a) && !::fstat(current.value, &b)
        && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool journal(int dir, const QJsonObject& value)
{
    // The old complete journal remains if the new write or fsync fails.
    ::unlinkat(dir, "journal.next", 0);
    Fd file(::openat(dir, "journal.next", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!regular(file.value, true)) return false;
    const auto data = QJsonDocument(value).toJson(QJsonDocument::Compact);
    return data.size() <= 4096 && bytes(file.value, data) && !::fsync(file.value)
        && !::renameat(dir, "journal.next", dir, "journal.json") && !::fsync(dir);
}
QJsonObject readJournal(int dir)
{
    Fd fd(::openat(dir, "journal.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!regular(fd.value, true)) return {};
    struct stat s{};
    if (::fstat(fd.value, &s) || s.st_size <= 0 || s.st_size > 4096) return {};
    QByteArray data(s.st_size, '\0');
    if (::read(fd.value, data.data(), data.size()) != data.size()) return {};
    const auto json = QJsonDocument::fromJson(data);
    if (!json.isObject()) return {};
    const auto value = json.object();
    const auto name = value["name"].toString();
    if (value["version"] != 1 || name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains(QChar::Null)) return {};
    return value;
}
bool copy(int source, int destination)
{
    char data[65536];
    off_t offset = 0;
    while (offset <= MaximumBytes) {
        const auto n = ::pread(source, data, sizeof(data), offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (!n) return true;
        if (offset + n > MaximumBytes || !bytes(destination, QByteArray::fromRawData(data, n))) return false;
        offset += n;
    }
    return false;
}
bool validName(const QByteArray& name)
{
    // Only directories minted by this application, never arbitrary children.
    const auto p = QByteArray(Prefix);
    if (!name.startsWith(p) || name.size() != p.size() + 16 + 1 + 32 || name[p.size() + 16] != '-') return false;
    for (qsizetype i = p.size(); i < name.size(); ++i) {
        if (i == p.size() + 16) continue;
        if (!(name[i] >= '0' && name[i] <= '9') && !(name[i] >= 'a' && name[i] <= 'f')) return false;
    }
    return true;
}
QByteArray targetPrefix(const QByteArray& name)
{
    return QByteArray(Prefix) + QCryptographicHash::hash(name, QCryptographicHash::Sha256).toHex().left(16) + '-';
}
}

FileStamp stamp(const struct stat& s, const std::string& digest)
{
    return {std::uint64_t(s.st_dev), std::uint64_t(s.st_ino), std::uint64_t(s.st_size),
        std::uint64_t(s.st_mtim.tv_sec) * 1000000000ULL + s.st_mtim.tv_nsec,
        std::uint64_t(s.st_ctim.tv_sec) * 1000000000ULL + s.st_ctim.tv_nsec, digest};
}
bool isRecoveryPath(const QString& path)
{
    const auto resolved = QFileInfo(path).canonicalFilePath();
    for (const auto& part : resolved.split('/')) if (validName(QFile::encodeName(part))) return true;
    return false;
}
bool same(const FileStamp& a, const FileStamp& b, bool moved)
{
    return a.valid() && b.valid() && a.device == b.device && a.inode == b.inode && a.size == b.size
        && a.digest == b.digest && a.modified == b.modified && (moved || a.changed == b.changed);
}
FileStamp fingerprint(int fd)
{
    struct stat before{}, after{};
    if (fd < 0 || ::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 || before.st_size > MaximumBytes) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buffer[65536];
    off_t offset = 0;
    while (offset < before.st_size) {
        const auto count = ::pread(fd, buffer, std::min<off_t>(sizeof(buffer), before.st_size - offset), offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return {};
        hash.addData(QByteArrayView(buffer, count)); offset += count;
    }
    if (::fstat(fd, &after)) return {};
    const auto digest = hash.result().toHex().toStdString();
    const auto a = stamp(before, digest), b = stamp(after, digest);
    return same(a, b) ? b : FileStamp{};
}

static SaveResult transaction(const QString& path, const FileStamp* expected, Operation serialize, Operation verify, const Observer& observer, bool archiveOnly, int* lease)
{
    const QFileInfo info(path);
    const auto folder = info.absolutePath();
    const auto name = QFile::encodeName(info.fileName());
    SaveResult result{SaveStatus::IoError, info.absoluteFilePath().toStdString(), {}, false, false, {}};
    if (name.isEmpty() || name == "." || name == ".." || name.contains('\0')) { result.status = SaveStatus::UnsafePath; return result; }
    Fd parent(::open(QFile::encodeName(folder).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parent.value < 0) return result;
    if (isRecoveryPath(actualPath(parent.value))) { result.status = SaveStatus::UnsafePath; return result; }
    auto fail = [&](SaveStatus code) { result.status = result.published ? SaveStatus::Uncertain : code; return result; };
    const auto lockName = QByteArray(".keepassxc-gnome-lock-") + QCryptographicHash::hash(name, QCryptographicHash::Sha256).toHex();
    Fd lock(::openat(parent.value, lockName.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (!regular(lock.value, true)) return fail(SaveStatus::UnsafePath);
    if (::flock(lock.value, LOCK_EX | LOCK_NB)) return fail(SaveStatus::Busy);
    Fd old(::openat(parent.value, name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (expected) {
        if (old.value < 0 && errno == ENOENT) return fail(SaveStatus::Conflict);
        struct stat permissions{};
        if (!regular(old.value) || ::fstat(old.value, &permissions) || !(permissions.st_mode & S_IWUSR))
            return fail(SaveStatus::UnsafePath);
        if (!same(*expected, fingerprint(old.value))) return fail(SaveStatus::Conflict);
    } else {
        struct stat existing{};
        if (!::fstatat(parent.value, name.constData(), &existing, AT_SYMLINK_NOFOLLOW)) return fail(SaveStatus::Exists);
        if (errno != ENOENT) return fail(SaveStatus::IoError);
    }
    const auto prefix = targetPrefix(name);
    unsigned records = 0, scanned = 0;
    DIR* listing = ::fdopendir(::dup(parent.value));
    if (!listing) return result;
    bool scanOk = true;
    while (true) {
        errno = 0;
        const auto* entry = ::readdir(listing);
        if (!entry) { scanOk = !errno; break; }
        if (++scanned > 100000) { records = MaximumRecords; break; }
        if (QByteArray(entry->d_name).startsWith(prefix)) ++records;
    }
    ::closedir(listing);
    if (!scanOk) return fail(SaveStatus::IoError);
    if (records >= MaximumRecords) return fail(SaveStatus::RecoveryFull);
    const auto transaction = prefix + QUuid::createUuid().toString(QUuid::Id128).toLatin1();
    if (::mkdirat(parent.value, transaction.constData(), 0700)) return result;
    Fd dir(::openat(parent.value, transaction.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!safeDirectory(dir.value)) return fail(SaveStatus::UnsafePath);
    result.recovery = (actualPath(parent.value) + '/' + QFile::decodeName(transaction)).toStdString();
    Fd active(::openat(dir.value, "lock", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!regular(active.value, true) || ::flock(active.value, LOCK_EX | LOCK_NB)) return result;
    QJsonObject state{{"version", 1}, {"name", QFile::decodeName(name)}, {"state", "prepared"},
                      {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                      {"previousHash", expected ? QString::fromStdString(expected->digest) : QString{}}};
    auto gate = [&](Stage stage) { return !observer || observer(stage); };
    if (!journal(dir.value, state) || ::fsync(parent.value) || !gate(Stage::Prepared)) return result;
    if (expected) {
        Fd backup(::openat(dir.value, "previous.kdbx", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (!regular(backup.value, true) || !copy(old.value, backup.value) || !gate(Stage::BackupSync)
            || ::fsync(backup.value)) return fail(SaveStatus::IoError);
        if (fingerprint(backup.value).digest != expected->digest || !same(*expected, fingerprint(old.value)))
            return fail(SaveStatus::Conflict);
    }
    Fd candidate(::openat(dir.value, "candidate.kdbx", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!regular(candidate.value, true)) return result;
    QFile device;
    if (!device.open(candidate.value, QIODevice::ReadWrite, QFileDevice::DontCloseHandle)) return result;
    if (!serialize(device) || !device.flush() || device.size() > MaximumBytes || !gate(Stage::Written))
        return fail(SaveStatus::SerializationError);
    if (!gate(Stage::FileSync) || ::fsync(candidate.value)) return result;
    if (!device.seek(0) || !verify(device) || !gate(Stage::Verified)) return fail(SaveStatus::VerificationError);
    const auto candidateStamp = fingerprint(candidate.value);
    if (!candidateStamp.valid()) return fail(SaveStatus::VerificationError);
    // Keep an independent encrypted copy even after candidate is renamed.
    Fd written(::openat(dir.value, "written.kdbx", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!regular(written.value, true) || !copy(candidate.value, written.value) || ::fsync(written.value)
        || fingerprint(written.value).digest != candidateStamp.digest) return result;
    state["candidateHash"] = QString::fromStdString(candidateStamp.digest);
    state["state"] = "ready";
    if (!journal(dir.value, state) || ::fsync(parent.value) || !gate(Stage::Ready)) return result;
    if (!directoryStillThere(parent.value, folder)) return fail(SaveStatus::UnsafePath);
    if (archiveOnly) {
        if (::unlinkat(dir.value, "candidate.kdbx", 0) || ::fsync(dir.value)) return result;
        state["state"] = "checkpoint";
        if (!journal(dir.value, state) || ::fsync(parent.value)) return result;
        const auto archivedStamp = fingerprint(written.value);
        if (!archivedStamp.valid() || archivedStamp.digest != candidateStamp.digest)
            return fail(SaveStatus::VerificationError);
        if (!directoryStillThere(parent.value, folder)) return fail(SaveStatus::UnsafePath);
        if (lease) {
            *lease = ::fcntl(active.value, F_DUPFD_CLOEXEC, 3);
            if (*lease < 0) return result;
        }
        result.target = result.recovery + "/written.kdbx";
        result.saved = archivedStamp;
        result.published = result.durable = true;
        result.status = SaveStatus::Committed;
        return result;
    }
    if (expected) {
        Fd current(::openat(parent.value, name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (!regular(current.value) || !same(*expected, fingerprint(current.value))) return fail(SaveStatus::Conflict);
    }
    if (!gate(Stage::FinalChecked)) return result;
    if (::renameat2(dir.value, "candidate.kdbx", parent.value, name.constData(), expected ? RENAME_EXCHANGE : RENAME_NOREPLACE))
        return fail(errno == EEXIST ? SaveStatus::Exists : SaveStatus::IoError);
    result.published = true;
    if (!gate(Stage::Published)) return fail(SaveStatus::Uncertain);
    bool conflict = false;
    if (expected) {
        Fd displaced(::openat(dir.value, "candidate.kdbx", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        conflict = !regular(displaced.value) || !same(*expected, fingerprint(displaced.value), true);
        // Keep both the independent pre-save backup and the exchanged inode.
        // An external process might still hold and write the latter.
        if (::renameat2(dir.value, "candidate.kdbx", dir.value, "displaced.kdbx", RENAME_NOREPLACE)) return fail(SaveStatus::Uncertain);
        if (displaced.value >= 0 && ::fsync(displaced.value)) return fail(SaveStatus::Uncertain);
    }
    if (!gate(Stage::DirectorySync) || ::fsync(dir.value) || ::fsync(parent.value)) return fail(SaveStatus::Uncertain);
    Fd published(::openat(parent.value, name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    const auto finalStamp = fingerprint(published.value);
    if (!regular(published.value, true) || finalStamp.digest != candidateStamp.digest
        || !directoryStillThere(parent.value, folder)) conflict = true;
    state["state"] = conflict ? "conflict" : "committed";
    if (!journal(dir.value, state) || ::fsync(parent.value) || !gate(Stage::Complete)) return fail(SaveStatus::Uncertain);
    result.saved = finalStamp;
    result.durable = true;
    result.status = conflict ? SaveStatus::Conflict : SaveStatus::Committed;
    return result;
}

SaveResult write(const QString& path, const FileStamp* expected, Operation serialize, Operation verify, const Observer& observer)
{
    return transaction(path, expected, std::move(serialize), std::move(verify), observer, false, nullptr);
}
SaveResult archive(const QString& path, Operation serialize, Operation verify, int* lease)
{
    if (lease) *lease = -1;
    return transaction(path, nullptr, std::move(serialize), std::move(verify), {}, true, lease);
}

std::vector<RecoveryRecord> recoveries(const QString& folder, bool& complete)
{
    complete = false;
    std::vector<RecoveryRecord> result;
    Fd parent(::open(QFile::encodeName(folder).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parent.value < 0) return result;
    DIR* listing = ::fdopendir(::dup(parent.value));
    if (!listing) return result;
    unsigned scanned = 0;
    bool bounded = true;
    while (true) {
        errno = 0;
        const auto* entry = ::readdir(listing);
        if (!entry) { if (errno) bounded = false; break; }
        if (++scanned > 100000) { bounded = false; break; }
        const QByteArray name(entry->d_name);
        if (!validName(name)) continue;
        if (result.size() >= 128) { bounded = false; break; }
        Fd dir(::openat(parent.value, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!safeDirectory(dir.value)) { bounded = false; continue; }
        Fd active(::openat(dir.value, "lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (!regular(active.value, true)) { bounded = false; continue; }
        if (::flock(active.value, LOCK_EX | LOCK_NB)) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) bounded = false;
            continue;
        }
        const auto state = readJournal(dir.value);
        if (state.isEmpty() || !name.startsWith(targetPrefix(QFile::encodeName(state["name"].toString())))) { bounded = false; continue; }
        RecoveryRecord record{(actualPath(parent.value) + '/' + QFile::decodeName(name)).toStdString(),
            (actualPath(parent.value) + '/' + state["name"].toString()).toStdString(), state["state"].toString().toStdString(), {}, state["created"].toString().toStdString()};
        for (const auto* file : {"written.kdbx", "candidate.kdbx", "previous.kdbx", "displaced.kdbx"}) {
            Fd data(::openat(dir.value, file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            if (data.value < 0 && errno == ENOENT) continue;
            if (!regular(data.value)) { bounded = false; continue; }
            const auto actual = fingerprint(data.value).digest;
            if (actual.empty()) bounded = false;
            const auto candidateHash = state["candidateHash"].toString().toStdString();
            const auto previousHash = state["previousHash"].toString().toStdString();
            const bool verified = actual.size() == 64 && (actual == candidateHash || actual == previousHash);
            record.files.push_back({record.directory + '/' + file, file, verified});
        }
        result.push_back(std::move(record));
    }
    ::closedir(listing);
    complete = bounded && directoryStillThere(parent.value, folder);
    return result;
}

bool discard(const QString& directory)
{
    const QFileInfo info(directory);
    const auto name = QFile::encodeName(info.fileName());
    if (!validName(name)) return false;
    Fd parent(::open(QFile::encodeName(info.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Fd dir(parent.value < 0 ? -1 : ::openat(parent.value, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!safeDirectory(dir.value)) return false;
    Fd active(::openat(dir.value, "lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!regular(active.value, true) || ::flock(active.value, LOCK_EX | LOCK_NB)) return false;
    const auto state = readJournal(dir.value);
    if (state.isEmpty() || !name.startsWith(targetPrefix(QFile::encodeName(state["name"].toString())))) return false;
    DIR* listing = ::fdopendir(::dup(dir.value));
    if (!listing) return false;
    const QList<QByteArray> allowed{"journal.json", "journal.next", "lock", "written.kdbx", "candidate.kdbx", "previous.kdbx", "displaced.kdbx"};
    QList<QByteArray> files;
    bool valid = true;
    while (true) {
        errno = 0;
        const auto* item = ::readdir(listing);
        if (!item) { if (errno) valid = false; break; }
        const QByteArray file(item->d_name);
        if (file == "." || file == "..") continue;
        Fd entry(::openat(dir.value, file.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (!allowed.contains(file) || !regular(entry.value)) { valid = false; break; }
        files.append(file);
    }
    ::closedir(listing);
    struct stat held{}, named{};
    auto stillNamed = [&] { return !::fstat(dir.value, &held)
        && !::fstatat(parent.value, name.constData(), &named, AT_SYMLINK_NOFOLLOW)
        && S_ISDIR(named.st_mode) && held.st_dev == named.st_dev && held.st_ino == named.st_ino; };
    if (!valid || !directoryStillThere(parent.value, info.absolutePath()) || !stillNamed()) return false;
    // Keep the journal and active lock until all encrypted payloads are gone.
    // Interrupted cleanup can then still list the remaining recovery copies.
    const QList<QByteArray> order{"candidate.kdbx", "written.kdbx", "previous.kdbx", "displaced.kdbx", "journal.next", "journal.json", "lock"};
    for (const auto& file : order)
        if (files.contains(file) && ::unlinkat(dir.value, file.constData(), 0)) return false;
    return !::fsync(dir.value) && stillNamed() && !::unlinkat(parent.value, name.constData(), AT_REMOVEDIR) && !::fsync(parent.value);
}
}
