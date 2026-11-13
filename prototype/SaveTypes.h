#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class SaveStatus {
    None, Committed, Exists, Conflict, UnsafePath, Busy, IoError,
    SerializationError, VerificationError, Uncertain, NoSource, RecoveryFull
};

struct FileStamp {
    std::uint64_t device = 0, inode = 0, size = 0, modified = 0, changed = 0;
    std::string digest;
    bool valid() const { return inode && digest.size() == 64; }
};

struct SaveResult {
    SaveStatus status = SaveStatus::None;
    std::string target;
    std::string recovery;
    bool published = false;
    bool durable = false;
    FileStamp saved;
    bool committed() const { return status == SaveStatus::Committed && published && durable; }
};

struct RecoveryFile {
    std::string path;
    std::string kind;
    bool hashMatches = false;
};
struct RecoveryRecord {
    std::string directory;
    std::string target;
    std::string state;
    std::vector<RecoveryFile> files;
    std::string created;
};
