#include "CoreSession.h"

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Metadata.h"
#include "core/Totp.h"
#include "crypto/kdf/Kdf.h"
#include "format/KeePass2.h"
#include "format/KeePass2Reader.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"
#include "gnome/ReadBudget.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <array>
#include <cstdio>
#include <unistd.h>

namespace
{
bool require(bool condition, const char* operation)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", operation);
    }
    return condition;
}

bool runCli(const QString& program, const QStringList& args, int passwordLines, QByteArray* output = nullptr,
            const QString& password = QString::fromLatin1(CoreSession::FixturePassword))
{
    QProcess cli;
    cli.start(program, args);
    if (!cli.waitForStarted()) {
        return false;
    }
    const QByteArray input = password.toUtf8() + '\n';
    cli.write(input.repeated(passwordLines));
    cli.closeWriteChannel();
    if (!cli.waitForFinished() || cli.exitStatus() != QProcess::NormalExit || cli.exitCode() != 0) {
        return false;
    }
    if (output) {
        *output = cli.readAllStandardOutput();
    }
    return true;
}

QByteArray fileHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result();
}

bool comparePreserved(const QString& source, const QString& copy, quint32 version)
{
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QString::fromLatin1(CoreSession::FixturePassword)));
    Database original;
    Database reopened;
    KeePass2Reader originalReader;
    KeePass2Reader copyReader;
    if (!require(originalReader.readDatabase(source, key, &original) && copyReader.readDatabase(copy, key, &reopened),
                 "upstream reader reopens both files")
        || !require(originalReader.version() == version && copyReader.version() == version, "header format preserved")
        || !require(original.kdf()->uuid() == reopened.kdf()->uuid(), "KDF preserved")) {
        return false;
    }

    const auto before = original.rootGroup()->entriesRecursive();
    if (!require(before.size() == 3 && reopened.rootGroup()->entriesRecursive().size() == before.size(),
                 "all three TOTP entries retained")) {
        return false;
    }
    for (const auto* entry : before) {
        const auto* other = reopened.rootGroup()->findEntryByUuid(entry->uuid());
        if (!require(other != nullptr, "entry UUID preserved")
            || !require(entry->hasTotp() && other->hasTotp(), "TOTP settings remain recognized")
            || !require(entry->attachments()->value(QStringLiteral("fixture.txt"))
                            == other->attachments()->value(QStringLiteral("fixture.txt")),
                        "attachment preserved")
            || !require(entry->autoTypeAssociations()->getAll() == other->autoTypeAssociations()->getAll(),
                        "Auto-Type preserved")
            || !require(entry->customData()->value(QStringLiteral("Phase2.Marker"))
                            == other->customData()->value(QStringLiteral("Phase2.Marker")),
                        "custom data preserved")) {
            return false;
        }
        for (const auto& field : entry->attributes()->keys()) {
            if (field != EntryAttributes::TitleKey
                && !require(other->attributes()->hasKey(field)
                                && entry->attributes()->value(field) == other->attributes()->value(field)
                                && entry->attributes()->isProtected(field) == other->attributes()->isProtected(field),
                            "raw attributes and protection flags preserved")) {
                return false;
            }
        }
    }
    const auto* edited = reopened.rootGroup()->findEntryByUuid(before.first()->uuid());
    const auto priorHistory = before.first()->historyItems();
    const auto savedHistory = edited->historyItems();
    if (!require(savedHistory.size() == priorHistory.size() + 1, "title edit added exactly one history item")) {
        return false;
    }
    for (qsizetype index = 0; index < priorHistory.size(); ++index) {
        if (!require(savedHistory[index]->title() == priorHistory[index]->title(), "existing history preserved")) {
            return false;
        }
    }
    return require(edited->title() == QStringLiteral("変更済み fixture"), "UTF-8 title saved")
           && require(savedHistory.last()->title() == before.first()->title(), "previous title retained");
}

bool checkFormat(const QString& cli, const QTemporaryDir& directory, FixtureFormat format, quint32 version)
{
    const auto name = QString::number(version);
    const auto source = directory.filePath(name + QStringLiteral(".kdbx"));
    const auto output = directory.filePath(name + QStringLiteral("-copy.kdbx"));
    const auto missingDirectory = directory.filePath(QStringLiteral("absent/fail.kdbx"));
    const auto dangling = directory.filePath(name + QStringLiteral("-dangling.kdbx"));
    const auto danglingTarget = directory.filePath(name + QStringLiteral("-absent.kdbx"));
    CoreSession session;
    if (!require(CoreSession::createFixture(source.toStdString(), format), "fixture created")
        || !require(!session.unlock(source.toStdString(), "wrong fixture password") && session.entries().empty(),
                    "wrong password leaves session locked")
        || !require(session.unlock(source.toStdString(), CoreSession::FixturePassword), "fixture unlocked")) {
        return false;
    }
    const auto rows = session.entries();
    const auto sourceHash = fileHash(source);
    if (!require(rows.size() == 3, "fixture titles listed")
        || !require(session.rename(rows.front().id, "変更済み fixture"), "fixture renamed")
        || !require(!session.saveCopy(missingDirectory.toStdString()), "write failure reported")
        || !require(!session.saveCopy(source.toStdString()), "existing destination refused")
        || !require(::symlink(QFile::encodeName(danglingTarget).constData(), QFile::encodeName(dangling).constData()) == 0
                        && !session.saveCopy(dangling.toStdString()) && QFileInfo(dangling).isSymLink()
                        && !QFile::exists(danglingTarget),
                    "dangling destination refused without following its link")
        || !require(session.saveCopy(output.toStdString()), "atomic copy saved after failure")
        || !require(!sourceHash.isEmpty() && sourceHash == fileHash(source), "original bytes unchanged")) {
        return false;
    }
    session.lock();
    QByteArray cliOutput;
    return require(session.entries().empty() && session.copyPassword(rows.front().id).empty(), "lock revokes access")
           && require(runCli(cli, {QStringLiteral("ls"), output}, 1, &cliOutput)
                          && cliOutput.contains(QByteArrayLiteral("変更済み fixture")),
                      "upstream CLI reads saved title")
           && require(session.unlock(output.toStdString(), CoreSession::FixturePassword), "saved copy unlocked")
           && comparePreserved(source, output, version);
}

bool checkPublicFixture(const QString& cli, const QString& fixtureDirectory, const QTemporaryDir& directory,
                        const QString& name, const QString& password, const char* knownRefusal = nullptr)
{
    std::printf("Public upstream fixture: %s\n", qPrintable(name));
    const auto original = fixtureDirectory + QLatin1Char('/') + name;
    const auto source = directory.filePath(name);
    const auto copy = directory.filePath(QStringLiteral("copy-") + name);
    const auto digest = fileHash(original);
    if (!require(!digest.isEmpty() && QFile::copy(original, source), "copy public fixture before opening")) {
        return false;
    }
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(password));
    Database before, after;
    KeePass2Reader first, second;
    QByteArray exported;
    if (!require(runCli(cli, {QStringLiteral("export"), source}, 1, &exported, password)
                    && exported.contains("<KeePassFile>"), "unmodified CLI accepts public fixture")
        || !require(first.readDatabase(source, key, &before), "read original public fixture model")) {
        return false;
    }
    CoreSession session;
    const bool opened = session.unlock(source.toStdString(), password.toStdString());
    if (knownRefusal) {
        // These remain compatibility limitations, not successful round trips.
        // Repeat the guarded read only for a diagnostic from public test data.
        ReadLimits limits;
        ReadStats stats;
        ReadFailure failure = ReadFailure::None;
        GnomeRead::Context context{limits, stats, failure, static_cast<quint64>(QFileInfo(source).size())};
        context.sourceVersion = first.version();
        Database rejected;
        KeePass2Reader diagnostic;
        GnomeRead::Scope scope(context);
        const bool read = diagnostic.readDatabase(source, key, &rejected);
        std::printf("KNOWN COMPATIBILITY LIMITATION: %s: %s\n", qPrintable(name), qPrintable(diagnostic.errorString()));
        return require(!opened && session.lastReadFailure() == ReadFailure::UnsupportedData
                           && !read && failure == ReadFailure::UnsupportedData
                           && diagnostic.errorString().contains(QString::fromLatin1(knownRefusal)),
                       "public fixture refused for the documented preservation limitation")
            && require(session.entries().empty() && session.groups().empty()
                           && !session.saveCopy(copy.toStdString()) && !QFile::exists(copy),
                       "refusal publishes no model and creates no saved file")
            && require(fileHash(original) == digest && fileHash(source) == digest, "refused public fixture bytes unchanged");
    }
    if (!opened) {
        std::fprintf(stderr, "Public fixture refused: %s, ReadFailure=%d\n", qPrintable(name),
                     static_cast<int>(session.lastReadFailure()));
    }
    if (!require(opened, "adapter accepts public fixture")
        || !require(session.saveCopy(copy.toStdString()), "save public fixture to a new file")) {
        return false;
    }
    session.lock();
    if (!require(runCli(cli, {QStringLiteral("export"), copy}, 1, &exported, password)
                    && exported.contains("<KeePassFile>"), "unmodified CLI accepts saved public fixture")
        || !require(second.readDatabase(copy, key, &after), "read saved public fixture model")
        || !require(before.cipher() == after.cipher() && before.kdf()->uuid() == after.kdf()->uuid()
                        && before.compressionAlgorithm() == after.compressionAlgorithm(),
                    "cipher, KDF and compression retained")
        || !require(before.metadata()->name() == after.metadata()->name()
                        && before.metadata()->description() == after.metadata()->description(),
                    "database name and description retained")
        || !require(before.rootGroup()->groupsRecursive(true).size() == after.rootGroup()->groupsRecursive(true).size()
                        && before.rootGroup()->entriesRecursive().size() == after.rootGroup()->entriesRecursive().size(),
                    "public fixture group and entry counts retained")) {
        return false;
    }
    for (const auto* group : before.rootGroup()->groupsRecursive(true)) {
        const auto* other = after.rootGroup()->findGroupByUuid(group->uuid());
        if (!require(other && group->name() == other->name() && group->notes() == other->notes(),
                     "group identity, name and notes retained")) return false;
        for (const auto* entry : group->entries()) {
            const auto* saved = other->findEntryByUuid(entry->uuid(), false);
            if (!require(entry->equals(saved), "complete entry data and existing history retained without editing")) {
                return false;
            }
        }
    }
    return require(fileHash(original) == digest && fileHash(source) == digest, "public fixture bytes unchanged");
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!require(argc == 3 && directory.isValid(), "test environment")
        || !require(CoreSession::initialize(), "upstream crypto self-test")) {
        return 1;
    }
    const auto cli = QString::fromLocal8Bit(argv[1]);
    const auto upstreamFile = directory.filePath(QStringLiteral("cli-generated.kdbx"));
    CoreSession session;
    if (!require(runCli(cli, {QStringLiteral("db-create"), QStringLiteral("-p"), upstreamFile}, 2), "CLI creates DB")
        || !require(runCli(cli, {QStringLiteral("add"), upstreamFile, QStringLiteral("CLI fixture")}, 1), "CLI adds entry")
        || !require(session.unlock(upstreamFile.toStdString(), CoreSession::FixturePassword)
                        && session.entries().size() == 1 && session.entries().front().title == "CLI fixture",
                    "adapter opens CLI-generated database")) {
        return 1;
    }
    session.lock();

    const std::array cases{
        std::pair{FixtureFormat::Kdbx3, KeePass2::FILE_VERSION_3_1},
        std::pair{FixtureFormat::Kdbx4, KeePass2::FILE_VERSION_4},
        std::pair{FixtureFormat::Kdbx41, KeePass2::FILE_VERSION_4_1},
    };
    for (const auto& [format, version] : cases) {
        if (!checkFormat(cli, directory, format, version)) {
            return 1;
        }
    }
    const auto fixtureDirectory = QString::fromLocal8Bit(argv[2]);
    const std::array publicFixtures{
        std::pair{QStringLiteral("NonAscii.kdbx"), QString::fromUtf8("\xce\x94\xc3\xb6\xd8\xb6")},
        std::pair{QStringLiteral("Compressed.kdbx"), QStringLiteral("")},
        std::pair{QStringLiteral("ProtectedStrings.kdbx"), QStringLiteral("masterpw")},
    };
    bool publicFixturesPassed = true;
    for (const auto& [name, password] : publicFixtures) {
        publicFixturesPassed = checkPublicFixture(cli, fixtureDirectory, directory, name, password) && publicFixturesPassed;
    }
    QTemporaryDir referenceDirectory;
    if (!require(referenceDirectory.isValid(), "private directory for official writer output")) return 1;
    struct LimitedFixture { QString name; QString password; const char* reason; };
    const std::array limitedFixtures{
        LimitedFixture{QStringLiteral("Format300.kdbx"), QStringLiteral("a"), "Unresolved model references"},
        LimitedFixture{QStringLiteral("Format400.kdbx"), QStringLiteral("t"), "Binary protection flags"},
    };
    for (const auto& fixture : limitedFixtures) {
        publicFixturesPassed = checkPublicFixture(cli, fixtureDirectory, directory, fixture.name, fixture.password,
                                                  fixture.reason) && publicFixturesPassed;
        // A separate experiment covers output written by the current official
        // CLI. It does not repair the originals or erase their refusal result.
        const auto referenceName = QStringLiteral("reference-") + fixture.name;
        const auto referencePath = referenceDirectory.filePath(referenceName);
        const bool prepared = QFile::copy(fixtureDirectory + QLatin1Char('/') + fixture.name, referencePath)
            && runCli(cli, {QStringLiteral("db-edit"), QStringLiteral("-p"), referencePath}, 3, nullptr, fixture.password);
        publicFixturesPassed = require(prepared, "unmodified CLI resaves a separate public fixture copy")
            && checkPublicFixture(cli, referenceDirectory.path(), directory, referenceName, fixture.password)
            && publicFixturesPassed;
    }
    if (!publicFixturesPassed) return 1;
    std::puts("PASS: CLI interoperability and KDBX 3.1 / 4.0 / 4.1 fixture preservation");
    std::puts("PASS: three original public fixtures and two official-CLI-resaved copies round trip");
    std::puts("LIMITATIONS: two original public fixtures remain unsupported; refusal and source preservation verified");
}
