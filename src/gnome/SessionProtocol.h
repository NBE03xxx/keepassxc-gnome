#pragma once

#include "CoreSession.h"
#include "gnome/Sensitive.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace SessionProtocol {
inline constexpr quint32 MaximumFrame = 16U << 20;
inline constexpr int Version = 5; // explicit password factor and optional key file
struct Error : std::runtime_error { Error() : std::runtime_error("Invalid worker protocol") {} };
struct SizeError : Error {};

inline ssize_t quietWrite(int fd, const char* bytes, std::size_t length)
{
    // Do not change the application's global SIGPIPE disposition. Suppress
    // only a newly generated SIGPIPE on this controller thread after EPIPE.
    sigset_t pipe, old, pending;
    sigemptyset(&pipe); sigaddset(&pipe, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &pipe, &old)) { errno = EIO; return -1; }
    if (sigpending(&pending)) {
        const auto error = errno;
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        errno = error;
        return -1;
    }
    const bool alreadyPending = sigismember(&pending, SIGPIPE);
    const auto result = ::write(fd, bytes, length);
    const auto error = errno;
    if (result < 0 && error == EPIPE && !alreadyPending) {
        const timespec zero{0, 0};
        while (sigtimedwait(&pipe, nullptr, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = error;
    return result;
}

inline QString string(const QJsonValue& value)
{
    if (!value.isString()) throw Error{};
    return value.toString();
}
inline std::string text(const QJsonValue& value) { return string(value).toStdString(); }
inline QString str(const std::string& value) { return QString::fromStdString(value); }

inline QJsonObject encode(const SaveResult& result)
{
    return {{"status", int(result.status)}, {"target", str(result.target)}, {"recovery", str(result.recovery)},
        {"published", result.published}, {"durable", result.durable}};
}
inline SaveResult saved(const QJsonValue& value)
{
    if (!value.isObject()) throw Error{};
    const auto v = value.toObject();
    const int code = v["status"].toInt(-1);
    if (code < 0 || code > int(SaveStatus::RecoveryFull) || !v["published"].isBool() || !v["durable"].isBool()) throw Error{};
    SaveResult result;
    result.status = SaveStatus(code); result.target = text(v["target"]); result.recovery = text(v["recovery"]);
    result.published = v["published"].toBool(); result.durable = v["durable"].toBool();
    if ((result.status == SaveStatus::Committed && !result.committed()) || (result.durable && !result.published)) throw Error{};
    return result;
}
inline QJsonObject encode(const RecoveryRecord& record)
{
    QJsonArray files;
    for (const auto& file : record.files) files.append(QJsonArray{str(file.path), str(file.kind), file.hashMatches});
    return {{"directory", str(record.directory)}, {"target", str(record.target)}, {"state", str(record.state)}, {"files", files}, {"created", str(record.created)}};
}
inline RecoveryRecord recovery(const QJsonValue& value)
{
    if (!value.isObject()) throw Error{};
    const auto v = value.toObject();
    RecoveryRecord result{text(v["directory"]), text(v["target"]), text(v["state"]), {}, text(v["created"])};
    if (!v["files"].isArray() || v["files"].toArray().size() > 4) throw Error{};
    for (const auto& file : v["files"].toArray()) {
        const auto a = file.toArray();
        if (a.size() != 3 || !a[2].isBool()) throw Error{};
        result.files.push_back({text(a[0]), text(a[1]), a[2].toBool()});
    }
    return result;
}

inline QJsonArray encode(const EntryDetails& item)
{
    return {str(item.id), str(item.group), str(item.title), str(item.username), str(item.password), str(item.url), str(item.notes), item.icon, item.customIcon};
}
inline EntryDetails details(const QJsonValue& value)
{
    if (!value.isArray() || value.toArray().size() != 9) throw Error{};
    const auto a = value.toArray();
    if (!a[7].isDouble() || a[7].toInt(-2) < -1 || !a[8].isBool()) throw Error{};
    return {text(a[0]), text(a[1]), text(a[2]), text(a[3]), text(a[4]), text(a[5]), text(a[6]), a[7].toInt(), a[8].toBool()};
}
inline bool mutation(const QString& op)
{
    return op == "addGroup" || op == "renameGroup" || op == "updateGroup" || op == "setGroupIcon" || op == "removeGroup" || op == "addEntry"
        || op == "updateEntry" || op == "removeEntry" || op == "rename";
}
inline QByteArray frame(const QJsonObject& object)
{
    auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const Sensitive::Erase clearJson{json.data(), std::size_t(json.size())};
    if (json.isEmpty() || quint64(json.size()) > MaximumFrame) throw SizeError{};
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(json.size(), bytes.data());
    bytes += json;
    return bytes;
}
inline bool transfer(int fd, char* bytes, std::size_t length, bool write)
{
    while (length) {
        const auto n = write ? quietWrite(fd, bytes, length) : ::read(fd, bytes, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n; length -= n;
    }
    return true;
}
inline bool send(int fd, const QJsonObject& value)
{
    auto bytes = frame(value);
    const Sensitive::Erase clearFrame{bytes.data(), std::size_t(bytes.size())};
    return transfer(fd, bytes.data(), bytes.size(), true);
}
inline bool receive(int fd, QJsonObject& value)
{
    char prefix[4];
    if (!transfer(fd, prefix, sizeof(prefix), false)) return false;
    const auto length = qFromLittleEndian<quint32>(prefix);
    if (!length || length > MaximumFrame) throw SizeError{};
    QByteArray bytes(length, '\0');
    const Sensitive::Erase clearFrame{bytes.data(), std::size_t(bytes.size())};
    if (!transfer(fd, bytes.data(), bytes.size(), false)) return false;
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) throw Error{};
    value = json.object();
    return true;
}
}
