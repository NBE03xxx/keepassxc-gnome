#pragma once
#include "ReadLimits.h"
#include "gnome/Sensitive.h"
#include "gnome/SafeSave.h"
#include "keys/FileKey.h"
#include <QBuffer>
#include <QFile>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace GnomeKeyFile {
// Bound and snapshot the local input once. Format detection, XML/checksum
// handling and key derivation belong exclusively to upstream FileKey.
inline bool load(const std::string& path, FileKey& key, std::uint64_t maximum, ReadFailure& failure)
{
    auto reject = [&](ReadFailure reason) { failure = reason; return false; };
    if (path.size() > 4096 || path.find('\0') != std::string::npos) return reject(ReadFailure::KeyFileIoError);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return reject(ReadFailure::KeyFileIoError);
    struct stat before{};
    if (::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0) {
        ::close(fd); return reject(ReadFailure::KeyFileIoError);
    }
    if (std::uint64_t(before.st_size) > maximum) { ::close(fd); return reject(ReadFailure::KeyFileTooLarge); }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd); return reject(ReadFailure::KeyFileIoError);
    }
    struct Snapshot {
        QByteArray bytes;
        ~Snapshot() { Sensitive::erase(bytes.data(), std::size_t(bytes.size())); }
    } snapshot;
    snapshot.bytes.reserve(before.st_size);
    char block[65536]; const Sensitive::Erase clearBlock{block, sizeof(block)};
    for (;;) {
        const auto left = std::uint64_t(before.st_size) - std::uint64_t(snapshot.bytes.size());
        const auto count = file.read(block, std::min<std::uint64_t>(sizeof(block), left + 1));
        if (count < 0) return reject(ReadFailure::KeyFileIoError);
        if (!count) break;
        if (std::uint64_t(count) > left) return reject(ReadFailure::KeyFileIoError);
        snapshot.bytes.append(block, count);
    }
    struct stat after{};
    if (::fstat(fd, &after)) return reject(ReadFailure::KeyFileIoError);
    const auto first = SafeSave::stamp(before, {}), last = SafeSave::stamp(after, {});
    if (after.st_size != snapshot.bytes.size() || first.device != last.device || first.inode != last.inode
        || first.size != last.size || first.modified != last.modified || first.changed != last.changed)
        return reject(ReadFailure::KeyFileIoError);
    file.close();
    QBuffer buffer(&snapshot.bytes);
    if (!buffer.open(QIODevice::ReadOnly) || !key.load(&buffer)) return reject(ReadFailure::InvalidKeyFile);
    return true;
}
}
