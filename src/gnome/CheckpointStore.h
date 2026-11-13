#pragma once
#include "CoreSession.h"
#include "gnome/SafeSave.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <deque>
#include <sys/stat.h>
#include <unistd.h>

class CheckpointStore {
public:
    bool enabled() const { return !logicalPath.empty(); }
    std::string latest() const { return records.empty() ? std::string{} : (records.back().path + "/written.kdbx").toStdString(); }
    bool enable(const QString& root, CoreSession& session) {
        if (enabled() || !QFileInfo(root).isAbsolute()) return false;
        struct stat s{};
        if (::lstat(QFile::encodeName(root), &s) || !S_ISDIR(s.st_mode) || s.st_uid != ::geteuid() || (s.st_mode & 0777) != 0700) return false;
        folder = root; device = s.st_dev; inode = s.st_ino;
        const auto name = QStringLiteral("unsaved-") + QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmssZ-")
            + QUuid::createUuid().toString(QUuid::Id128) + ".kdbx";
        logicalPath = (root + '/' + name).toStdString();
        return commit(session);
    }
    bool commit(CoreSession& session) {
        struct stat s{};
        if (!enabled() || ::lstat(QFile::encodeName(folder), &s) || !S_ISDIR(s.st_mode) || s.st_uid != ::geteuid()
            || (s.st_mode & 0777) != 0700 || s.st_dev != device || s.st_ino != inode) return false;
        int lease = -1;
        const auto saved = session.checkpoint(logicalPath, &lease);
        if (!saved.committed() || lease < 0) { if (lease >= 0) ::close(lease); return false; }
        records.emplace_back(QString::fromStdString(saved.recovery), lease);
        // Only this worker's own completed checkpoints are retired. Never
        // prune other sessions, user saves, an active record or unknown files.
        while (records.size() > 2) {
            records.front().release();
            if (!SafeSave::discard(records.front().path)) break;
            records.pop_front();
        }
        return true;
    }

private:
    QString folder;
    std::string logicalPath;
    dev_t device{};
    ino_t inode{};
    struct Record {
        QString path; int lease;
        Record(QString p, int fd) : path(std::move(p)), lease(fd) {}
        Record(const Record&) = delete;
        Record& operator=(const Record&) = delete;
        void release() { if (lease >= 0) { ::close(lease); lease = -1; } }
        ~Record() { release(); }
    };
    std::deque<Record> records;
};
