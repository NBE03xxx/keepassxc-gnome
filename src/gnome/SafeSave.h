#pragma once

#include "SaveTypes.h"
#include <QFile>
#include <QString>
#include <functional>
#include <sys/stat.h>

namespace SafeSave {
inline constexpr auto Prefix = ".keepassxc-gnome-save-";
enum class Stage { Prepared, BackupSync, Written, FileSync, Verified, Ready,
                   FinalChecked, Published, DirectorySync, Complete };
// C++ dependency injection for crash/I/O/race tests, never exposed through the
// application protocol, command line, environment or configuration.
using Observer = std::function<bool(Stage)>;
using Operation = std::function<bool(QFile&)>;
FileStamp stamp(const struct stat& status, const std::string& digest);
FileStamp fingerprint(int fd);
bool same(const FileStamp& a, const FileStamp& b, bool moved = false);
bool isRecoveryPath(const QString& path);
SaveResult write(const QString& path, const FileStamp* expected, Operation serialize, Operation verify,
                 const Observer& observer = {});
// Durable encrypted recovery record without replacing/publishing a DB path.
SaveResult archive(const QString& logicalPath, Operation serialize, Operation verify, int* lease = nullptr);
std::vector<RecoveryRecord> recoveries(const QString& folder, bool& complete);
bool discard(const QString& directory);
}
