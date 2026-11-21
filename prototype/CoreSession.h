#pragma once

#include "ReadLimits.h"
#include "SaveTypes.h"

#include <memory>
#include <cstring>
#include <string>
#include <vector>

class Database;
class Entry;
class BrowserSession;
class QJsonArray;
class QJsonObject;
class QString;

enum class FixtureFormat { Kdbx3, Kdbx4, Kdbx41 };

// Absence of the password factor differs from an explicitly empty password.
struct UnlockCredentials
{
    bool usePassword = true;
    std::string password;
    std::string keyFile;
    void clearSensitive() {
        if (!password.empty()) ::explicit_bzero(password.data(), password.size());
        if (!keyFile.empty()) ::explicit_bzero(keyFile.data(), keyFile.size());
        password.clear(); keyFile.clear(); usePassword = false;
    }
};

struct EntrySummary
{
    std::string id;
    std::string title;
    std::string username;
    std::string group;
    int icon = 0;
    bool customIcon = false;
};

struct GroupSummary
{
    std::string id;
    std::string name;
    unsigned depth = 0;
    int icon = 48;
    bool customIcon = false;
};

struct EntryDetails
{
    std::string id;
    std::string group;
    std::string title;
    std::string username;
    std::string password;
    std::string url;
    std::string notes;
    int icon = -1; // -1 preserves the existing icon (including custom icons).
    bool customIcon = false;
};

class CoreSession
{
public:
    static constexpr auto FixturePassword = "phase2-demo";

    CoreSession();
    ~CoreSession();

    static bool initialize();
    static bool createFixture(const std::string& path, FixtureFormat format);

    bool unlock(const std::string& path, const std::string& password, const ReadLimits& limits = {});
    bool unlock(const std::string& path, const UnlockCredentials& credentials, const ReadLimits& limits = {});
    ReadFailure lastReadFailure() const { return readFailure; }
    ReadStats lastReadStats() const { return readStats; }
    bool create(const std::string& name, const std::string& password);
    void lock();
    std::vector<EntrySummary> entries() const;
    std::vector<EntrySummary> search(const std::string& query, const std::string& group = {}) const;
    std::vector<GroupSummary> groups() const;
    std::string addGroup(const std::string& parent, const std::string& name);
    bool renameGroup(const std::string& id, const std::string& name);
    bool updateGroup(const std::string& id, const std::string& name, int icon = -1);
    bool setGroupIcon(const std::string& id, int icon);
    bool removeGroup(const std::string& id);
    std::string addEntry(const std::string& group, const EntryDetails& details);
    bool entry(const std::string& id, EntryDetails& details) const;
    bool updateEntry(const EntryDetails& details);
    bool removeEntry(const std::string& id);
    static std::string generatePassword(unsigned length = 24);
    bool rename(const std::string& id, const std::string& title);
    bool saveCopy(const std::string& path);
    bool saveAs(const std::string& path);
    bool save();
    bool hasSource() const { return !sourcePath.empty() && sourceStamp.valid(); }
    bool isModified() const;
    SaveResult lastSaveResult() const { return saveResult; }
    SaveResult checkpoint(const std::string& logicalPath, int* lease = nullptr);
    std::string recoveryDirectory() const { return recoveryPath; }
    std::string copyUsername(const std::string& id) const;
    std::string copyPassword(const std::string& id) const;

private:
    friend class BrowserSession;
    QString browserDatabaseHash() const;
    bool addBrowserAssociation(const QString& name, const QString& key);
    bool matchesBrowserAssociation(const QString& name, const QString& key) const;
    QJsonArray browserLoginChoices(const QString& origin, int maximum) const;
    QJsonObject browserLogin(const QString& origin, const QString& id) const;
    bool persist(const std::string& path, bool replace, bool associate);
    std::string sourcePath;
    std::string recoveryPath;
    FileStamp sourceStamp;
    SaveResult saveResult;
    Entry* findEntry(const std::string& id) const;
    std::unique_ptr<Database> database;
    ReadFailure readFailure = ReadFailure::None;
    ReadStats readStats;
};
