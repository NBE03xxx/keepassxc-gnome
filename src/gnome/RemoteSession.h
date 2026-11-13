#pragma once

#include "CoreSession.h"
#include "gnome/ProcessPolicy.h"
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <memory>

// All methods except cancel()/processId() belong to one controller thread.
// Secrets and the database model stay in a private, disposable worker process.
class RemoteSession
{
public:
    using Cancellation = std::shared_ptr<std::atomic<std::uint64_t>>;
    explicit RemoteSession(QString executable = {}, ProcessLimits limits = {}, Cancellation cancellation = {});
    ~RemoteSession();
    RemoteSession(const RemoteSession&) = delete;
    RemoteSession& operator=(const RemoteSession&) = delete;

    void beginTask(std::uint64_t epoch);
    void cancel();
    long processId() const;
    ReadFailure fatalFailure() const;
    ReadFailure lastReadFailure() const;
    QJsonObject enforcedLimits() const;
    bool unlock(const std::string& path, const std::string& password);
    bool create(const std::string& name, const std::string& password);
    bool enableRecovery(const std::string& privateFolder);
    std::string lastCheckpoint() const;
    void lock();
    bool fullyStopped() const;
    std::vector<GroupSummary> groups() const;
    std::vector<EntrySummary> entries() const { return search({}); }
    std::vector<EntrySummary> search(const std::string& query, const std::string& group = {}) const;
    bool entry(const std::string& id, EntryDetails& details) const;
    std::string addGroup(const std::string& parent, const std::string& name);
    bool renameGroup(const std::string& id, const std::string& name);
    bool removeGroup(const std::string& id);
    std::string addEntry(const std::string& group, const EntryDetails& details);
    bool updateEntry(const EntryDetails& details);
    bool removeEntry(const std::string& id);
    bool rename(const std::string& id, const std::string& title);
    bool saveCopy(const std::string& path);
    bool saveAs(const std::string& path);
    bool save();
    bool hasSource() const;
    bool isModified() const;
    std::string recoveryDirectory() const;
    SaveResult lastSaveResult() const;
    std::vector<RecoveryRecord> recoveries(const std::string& folder, bool& complete);
    bool discardRecovery(const std::string& directory);
    std::string copyUsername(const std::string& id) const;
    std::string copyPassword(const std::string& id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
