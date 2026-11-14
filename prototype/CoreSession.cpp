#include "CoreSession.h"

#include "core/AutoTypeAssociations.h"
#include "core/Bootstrap.h"
#include "core/CustomData.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/EntrySearcher.h"
#include "core/Metadata.h"
#include "core/PasswordGenerator.h"
#include "core/Totp.h"
#include "crypto/Crypto.h"
#include "format/KeePass2.h"
#include "format/KeePass2Reader.h"
#include "format/KeePass2Writer.h"
#include "crypto/Random.h"
#include "gnome/SafeSave.h"
#include <QCryptographicHash>
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"
#include "gnome/ReadBudget.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QBuffer>

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
QSharedPointer<CompositeKey> keyFor(const std::string& password)
{
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QString::fromStdString(password)));
    return key;
}

SaveResult writeDatabase(Database& database, const QString& path, const FileStamp* expected, bool archive = false, int* lease = nullptr)
{
    const bool modified = database.isModified(), nonDataChanges = database.hasNonDataChanges();
    database.setEmitModified(false);
    // The same random slug policy as upstream Database::saveAs (no crypto/KDBX implementation here).
    database.metadata()->customData()->set(CustomData::RandomSlug,
        Random::instance()->randomArray(Random::instance()->randomUIntRange(64, 512)).toHex());
    auto serialize = [&](QFile& file) {
        KeePass2Writer writer;
        return writer.writeDatabase(&file, &database) && !writer.hasError();
    };
    auto verify = [&](QFile& file) {
        ReadLimits limits;
        ReadStats stats;
        ReadFailure failure = ReadFailure::None;
        GnomeRead::Context context{limits, stats, failure, static_cast<quint64>(file.size())};
        GnomeRead::Scope scope(context);
        KeePass2Reader header;
        Database headerData;
        if (!header.readDatabase(&file, {}, &headerData) || header.version() > KeePass2::FILE_VERSION_MAX
            || failure != ReadFailure::None) return false;
        context.sourceVersion = header.version();
        Database verified;
        verified.setEmitModified(false);
        KeePass2Reader reader;
        return file.seek(0) && reader.readDatabase(&file, database.key(), &verified)
            && failure == ReadFailure::None;
    };
    auto result = archive ? SafeSave::archive(path, serialize, verify, lease) : SafeSave::write(path, expected, serialize, verify);
    if (modified) database.markAsModified(); else database.markAsClean();
    if (nonDataChanges && !database.hasNonDataChanges()) database.markNonDataChange();
    database.setEmitModified(true);
    return result;
}
}

CoreSession::CoreSession() = default;

CoreSession::~CoreSession()
{
    lock();
}

bool CoreSession::create(const std::string& name, const std::string& password)
{
    if (database || name.empty() || password.empty()) {
        return false;
    }
    auto candidate = std::make_unique<Database>();
    candidate->metadata()->setName(QString::fromStdString(name));
    candidate->rootGroup()->setName(QString::fromStdString(name));
    if (!candidate->setKey(keyFor(password))
        || !candidate->changeKdf(KeePass2::uuidToKdf(KeePass2::KDF_ARGON2ID))) {
        return false;
    }
    candidate->setFormatVersion(KeePass2::FILE_VERSION_4_1);
    database = std::move(candidate);
    return true;
}

bool CoreSession::initialize()
{
    Bootstrap::disableCoreDumps();
    return Crypto::init();
}

bool CoreSession::createFixture(const std::string& path, FixtureFormat format)
{
    Database fixture;
    if (!fixture.setKey(keyFor(FixturePassword))) {
        return false;
    }
    if (format != FixtureFormat::Kdbx3
        && !fixture.changeKdf(KeePass2::uuidToKdf(KeePass2::KDF_ARGON2ID))) {
        return false;
    }
    if (format == FixtureFormat::Kdbx41) {
        fixture.setFormatVersion(KeePass2::FILE_VERSION_4_1);
    }

    for (const auto* title : {"合成 OTP URI", "合成 Legacy", "合成 KeePass 2"}) {
        auto* entry = new Entry();
        entry->setUuid(QUuid::createUuid());
        entry->setGroup(fixture.rootGroup());
        entry->setTitle(QString::fromUtf8(title));
        entry->setUsername(QStringLiteral("phase2-fixture"));
        entry->setPassword(QStringLiteral("synthetic-entry-password"));
        entry->attachments()->set(QStringLiteral("fixture.txt"), QByteArrayLiteral("fixture attachment"));
        entry->autoTypeAssociations()->add({QStringLiteral("Synthetic window"), QStringLiteral("{USERNAME}{ENTER}")});
        entry->attributes()->set(EntryAttributes::KPEX_PASSKEY_CREDENTIAL_ID, QStringLiteral("synthetic-id"), true);
        if (format != FixtureFormat::Kdbx3) {
            entry->customData()->set(QStringLiteral("Phase2.Marker"), QStringLiteral("preserve this value"));
        }

        const auto seed = QStringLiteral("JBSWY3DPEHPK3PXP");
        if (entry->title() == QStringLiteral("合成 OTP URI")) {
            entry->setTotp(Totp::createSettings(seed));
        } else if (entry->title() == QStringLiteral("合成 Legacy")) {
            entry->setTotp(Totp::createSettings(seed, 6, 30, Totp::LEGACY));
        } else {
            entry->attributes()->set(Totp::KP2_TOTP_SECRET, seed, true);
        }
    }

    return writeDatabase(fixture, QString::fromStdString(path), nullptr).committed();
}

bool CoreSession::unlock(const std::string& path, const std::string& password, const ReadLimits& limits)
{
    readStats = {};
    readFailure = ReadFailure::None;
    if (database) {
        readFailure = ReadFailure::InvalidInput;
        return false;
    }
    const ReadLimits maximum;
    for (auto member : {&ReadLimits::fileBytes, &ReadLimits::headerBytes, &ReadLimits::headerFields,
                        &ReadLimits::outerFieldBytes, &ReadLimits::dictionaryEntries, &ReadLimits::dictionaryNameBytes,
                        &ReadLimits::innerHeaderBytes, &ReadLimits::innerFields, &ReadLimits::innerFieldBytes,
                        &ReadLimits::blockBytes, &ReadLimits::expandedBytes, &ReadLimits::aesRounds,
                        &ReadLimits::argonMemoryKiB, &ReadLimits::argonRounds, &ReadLimits::argonLanes,
                        &ReadLimits::argonWorkKiB, &ReadLimits::xmlDepth, &ReadLimits::xmlElements,
                        &ReadLimits::modelItems, &ReadLimits::groupObjects, &ReadLimits::entryObjects,
                        &ReadLimits::historyItems, &ReadLimits::binaryItems, &ReadLimits::attachmentReferences,
                        &ReadLimits::customDataItems, &ReadLimits::customIcons, &ReadLimits::deletedObjects,
                        &ReadLimits::autoTypeAssociations, &ReadLimits::stringFields}) {
        if (!(limits.*member) || limits.*member > maximum.*member) {
            readFailure = ReadFailure::InvalidInput;
            return false;
        }
    }
    // Open once without blocking on a FIFO; only regular local files qualify.
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) { readFailure = ReadFailure::IoError; return false; }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        ::close(descriptor);
        readFailure = ReadFailure::IoError;
        return false;
    }
    if (static_cast<quint64>(status.st_size) > limits.fileBytes) {
        ::close(descriptor);
        readFailure = ReadFailure::ResourceLimit;
        return false;
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        readFailure = ReadFailure::IoError;
        return false;
    }
    QByteArray encrypted;
    encrypted.reserve(status.st_size);
    char buffer[65536];
    while (true) {
        const auto remaining = limits.fileBytes - static_cast<quint64>(encrypted.size());
        const auto count = file.read(buffer, std::min<quint64>(sizeof(buffer), remaining + 1));
        if (count < 0) { readFailure = ReadFailure::IoError; return false; }
        if (!count) break;
        if (static_cast<quint64>(count) > remaining) { readFailure = ReadFailure::ResourceLimit; return false; }
        encrypted.append(buffer, count);
    }
    struct stat after{};
    FileStamp baseline;
    if (!::fstat(descriptor, &after)) {
        const auto digest = QCryptographicHash::hash(encrypted, QCryptographicHash::Sha256).toHex().toStdString();
        const auto beforeStamp = SafeSave::stamp(status, digest), afterStamp = SafeSave::stamp(after, digest);
        struct stat named{};
        if (SafeSave::same(beforeStamp, afterStamp) && after.st_size == encrypted.size()
            && after.st_uid == ::geteuid() && after.st_nlink == 1 && !::lstat(path.c_str(), &named)
            && S_ISREG(named.st_mode) && named.st_dev == after.st_dev && named.st_ino == after.st_ino)
            baseline = afterStamp;
    }
    file.close();
    QBuffer snapshot(&encrypted);
    if (!snapshot.open(QIODevice::ReadOnly)) { readFailure = ReadFailure::IoError; return false; }
    GnomeRead::Context context{limits, readStats, readFailure, static_cast<quint64>(encrypted.size())};
    GnomeRead::Scope scope(context);
    KeePass2Reader header;
    Database headerData;
    if (!header.readDatabase(&snapshot, {}, &headerData) || header.version() > KeePass2::FILE_VERSION_MAX
        || readFailure != ReadFailure::None) {
        if (readFailure == ReadFailure::None) readFailure = ReadFailure::InvalidInput;
        return false;
    }

    context.sourceVersion = header.version();
    auto candidate = std::make_unique<Database>();
    candidate->setEmitModified(false);
    KeePass2Reader reader;
    snapshot.seek(0);
    if (!reader.readDatabase(&snapshot, keyFor(password), candidate.get()) || readFailure != ReadFailure::None) {
        if (readFailure == ReadFailure::None) readFailure = ReadFailure::InvalidInput;
        return false;
    }
    // The KDBX4 reader's setKdf() can recalculate the model's version before
    // the payload is read. Keep the file version when saving this fixture.
    candidate->setFormatVersion(header.version());
    // Keep the editing snapshot separate from the read-only source.
    candidate->setFilePath({});
    candidate->markAsClean();
    candidate->setEmitModified(true);
    database = std::move(candidate);
    const auto absolute = QFileInfo(QString::fromStdString(path)).absoluteFilePath();
    // Recovery records are immutable input copies; recovered models always Save As.
    if (SafeSave::isRecoveryPath(absolute)) {
        recoveryPath = QFileInfo(QFileInfo(absolute).canonicalFilePath()).absolutePath().toStdString();
    } else {
        sourcePath = absolute.toStdString();
        sourceStamp = baseline;
    }
    return true;
}

void CoreSession::lock()
{
    sourcePath.clear();
    recoveryPath.clear();
    sourceStamp = {};
    saveResult = {};
    if (database) {
        // The upstream FileWatcher can still be calculating a checksum.
        QThreadPool::globalInstance()->waitForDone();
        database.reset();
    }
}

std::vector<EntrySummary> CoreSession::entries() const
{
    return search({});
}

std::vector<EntrySummary> CoreSession::search(const std::string& query, const std::string& group) const
{
    std::vector<EntrySummary> result;
    if (!database) {
        return result;
    }
    auto* base = group.empty() ? database->rootGroup()
                              : database->rootGroup()->findGroupByUuid(QUuid(QString::fromStdString(group)));
    if (!base) {
        return result;
    }
    EntrySearcher searcher(false, true);
    const auto matches = query.empty() ? (group.empty() ? base->entriesRecursive() : base->entries())
                                      : searcher.search(QString::fromStdString(query), base);
    for (const auto* item : matches) {
        if (item->isRecycled()) {
            continue;
        }
        result.push_back({item->uuid().toString().toStdString(), item->title().toStdString(),
                          item->username().toStdString(), item->group()->uuid().toString().toStdString(), item->iconNumber(), !item->iconUuid().isNull()});
    }
    return result;
}

std::vector<GroupSummary> CoreSession::groups() const
{
    std::vector<GroupSummary> result;
    if (!database) {
        return result;
    }
    for (const auto* group : database->rootGroup()->groupsRecursive(true)) {
        if (group->isRecycled() || group == database->metadata()->recycleBin()) {
            continue;
        }
        unsigned depth = 0;
        for (const auto* parent = group->parentGroup(); parent; parent = parent->parentGroup()) {
            ++depth;
        }
        result.push_back({group->uuid().toString().toStdString(), group->name().toStdString(), depth, group->iconNumber(), !group->iconUuid().isNull()});
    }
    return result;
}

std::string CoreSession::addGroup(const std::string& parent, const std::string& name)
{
    if (!database || name.empty()) {
        return {};
    }
    auto* target = parent.empty() ? database->rootGroup()
                                 : database->rootGroup()->findGroupByUuid(QUuid(QString::fromStdString(parent)));
    if (!target || target->isRecycled() || target == database->metadata()->recycleBin()) {
        return {};
    }
    auto* group = new Group;
    group->setUuid(QUuid::createUuid());
    group->setName(QString::fromStdString(name));
    group->setParent(target);
    return group->uuid().toString().toStdString();
}

bool CoreSession::renameGroup(const std::string& id, const std::string& name)
{
    return updateGroup(id, name);
}

bool CoreSession::updateGroup(const std::string& id, const std::string& name, int icon)
{
    // Validate both fields before either mutation. One worker request also
    // means one protected checkpoint for the complete group edit.
    if (!database || name.empty() || icon < -1 || icon > 68) {
        return false;
    }
    auto* group = database->rootGroup()->findGroupByUuid(QUuid(QString::fromStdString(id)));
    if (!group || group->isRecycled() || group == database->metadata()->recycleBin()) {
        return false;
    }
    group->setName(QString::fromStdString(name));
    if (icon >= 0) group->setIcon(icon);
    return true;
}

bool CoreSession::setGroupIcon(const std::string& id, int icon)
{
    if (!database || icon < 0 || icon > 68) return false;
    auto* group = database->rootGroup()->findGroupByUuid(QUuid(QString::fromStdString(id)));
    if (!group || group->isRecycled() || group == database->metadata()->recycleBin()) return false;
    group->setIcon(icon);
    return true;
}

bool CoreSession::removeGroup(const std::string& id)
{
    if (!database) {
        return false;
    }
    auto* group = database->rootGroup()->findGroupByUuid(QUuid(QString::fromStdString(id)));
    if (!group || group == database->rootGroup() || group == database->metadata()->recycleBin()
        || group->isRecycled()) {
        return false;
    }
    database->recycleGroup(group);
    return true;
}

std::string CoreSession::addEntry(const std::string& group, const EntryDetails& details)
{
    if (!database || details.title.empty() || details.icon < -1 || details.icon > 68) {
        return {};
    }
    auto* target = group.empty() ? database->rootGroup()
                                : database->rootGroup()->findGroupByUuid(QUuid(QString::fromStdString(group)));
    if (!target || target->isRecycled() || target == database->metadata()->recycleBin()) {
        return {};
    }
    auto* item = new Entry;
    item->setUuid(QUuid::createUuid());
    item->setTitle(QString::fromStdString(details.title));
    item->setUsername(QString::fromStdString(details.username));
    item->setPassword(QString::fromStdString(details.password));
    item->setUrl(QString::fromStdString(details.url));
    item->setNotes(QString::fromStdString(details.notes));
    if (details.icon >= 0) item->setIcon(details.icon);
    item->setGroup(target);
    return item->uuid().toString().toStdString();
}

bool CoreSession::entry(const std::string& id, EntryDetails& details) const
{
    const auto* item = findEntry(id);
    if (!item || item->isRecycled()) {
        details = {};
        return false;
    }
    details = {item->uuid().toString().toStdString(), item->group()->uuid().toString().toStdString(),
               item->title().toStdString(), item->username().toStdString(), item->password().toStdString(),
               item->url().toStdString(), item->notes().toStdString(), item->iconNumber(), !item->iconUuid().isNull()};
    return true;
}

bool CoreSession::updateEntry(const EntryDetails& details)
{
    auto* item = findEntry(details.id);
    if (!item || item->isRecycled() || details.title.empty() || details.icon < -1 || details.icon > 68) {
        return false;
    }
    item->beginUpdate();
    item->setTitle(QString::fromStdString(details.title));
    item->setUsername(QString::fromStdString(details.username));
    item->setPassword(QString::fromStdString(details.password));
    item->setUrl(QString::fromStdString(details.url));
    item->setNotes(QString::fromStdString(details.notes));
    if (details.icon >= 0 && !details.customIcon) item->setIcon(details.icon);
    item->endUpdate();
    return true;
}

bool CoreSession::removeEntry(const std::string& id)
{
    auto* item = findEntry(id);
    if (!item || item->isRecycled()) {
        return false;
    }
    database->recycleEntry(item);
    return true;
}

std::string CoreSession::generatePassword(unsigned length)
{
    if (length < 8 || length > 128) {
        return {};
    }
    PasswordGenerator generator;
    generator.setLength(length);
    generator.setCharClasses(PasswordGenerator::DefaultCharset | PasswordGenerator::SpecialCharacters);
    generator.setFlags(PasswordGenerator::DefaultFlags);
    return generator.isValid() ? generator.generatePassword().toStdString() : std::string{};
}

Entry* CoreSession::findEntry(const std::string& id) const
{
    if (!database) {
        return nullptr;
    }
    return database->rootGroup()->findEntryByUuid(QUuid(QString::fromStdString(id)));
}

bool CoreSession::rename(const std::string& id, const std::string& title)
{
    auto* entry = findEntry(id);
    if (!entry || entry->isRecycled() || title.empty()) {
        return false;
    }
    entry->beginUpdate();
    entry->setTitle(QString::fromStdString(title));
    entry->endUpdate();
    return true;
}

bool CoreSession::saveCopy(const std::string& path)
{
    return persist(path, false, false);
}

bool CoreSession::isModified() const { return database && database->isModified(); }
SaveResult CoreSession::checkpoint(const std::string& path, int* lease)
{
    if (!database) { SaveResult result; result.status = SaveStatus::NoSource; return result; }
    return writeDatabase(*database, QString::fromStdString(path), nullptr, true, lease);
}
bool CoreSession::saveAs(const std::string& path) { return persist(path, false, true); }
bool CoreSession::save()
{
    if (!hasSource()) { saveResult = {}; saveResult.status = SaveStatus::NoSource; return false; }
    return persist(sourcePath, true, true);
}
bool CoreSession::persist(const std::string& path, bool replace, bool associate)
{
    saveResult = {};
    if (!database) { saveResult.status = SaveStatus::NoSource; return false; }
    saveResult = writeDatabase(*database, QString::fromStdString(path), replace ? &sourceStamp : nullptr);
    if (!saveResult.committed()) return false;
    if (associate) {
        sourcePath = saveResult.target;
        sourceStamp = saveResult.saved;
        recoveryPath.clear();
        database->markAsClean();
    }
    return true;
}

std::string CoreSession::copyUsername(const std::string& id) const
{
    const auto* entry = findEntry(id);
    return entry && !entry->isRecycled() ? entry->resolveMultiplePlaceholders(entry->username()).toStdString() : std::string{};
}

std::string CoreSession::copyPassword(const std::string& id) const
{
    const auto* entry = findEntry(id);
    return entry && !entry->isRecycled() ? entry->resolveMultiplePlaceholders(entry->password()).toStdString() : std::string{};
}
