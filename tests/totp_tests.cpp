#include "gnome/RemoteSession.h"
#include "core/Base32.h"
#include "core/Clock.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "core/Totp.h"
#include "format/KeePass2Reader.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace
{
using Status = TotpCode::Status;
constexpr auto TestKey = "JBSWY3DPEHPK3PXP";

void check(bool condition, const char* label)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
}

class TestClock final : public Clock
{
public:
    static void at(qint64 millis) { setInstance(new TestClock(millis)); }
    static void restore() { resetInstance(); }

private:
    explicit TestClock(qint64 millis) : millis(millis) {}
    QDateTime currentDateTimeImpl() const override { return datetime(millis); }
    qint64 millis;
};

void checkKnownCodes()
{
    CoreSession session;
    check(session.create("Synthetic", CoreSession::FixturePassword), "create synthetic model");
    EntryDetails entry;
    entry.title = "RFC 6238";
    const auto id = session.addEntry(session.groups().front().id, entry);
    check(!id.empty() && session.totpCode(id).status == Status::NotConfigured, "entry initially has no TOTP");
    TestClock::at(59999);
    const struct { const char* algorithm; const char* key; const char* expected; } vectors[]{
        {"SHA1", "12345678901234567890", "94287082"},
        {"SHA256", "12345678901234567890123456789012", "46119246"},
        {"SHA512", "1234567890123456789012345678901234567890123456789012345678901234", "90693936"},
    };
    for (const auto& vector : vectors) {
        const auto uri = QByteArray("otpauth://totp/Test%20Account?issuer=Example&digits=8&period=30&algorithm=")
            + vector.algorithm + "&secret=" + Base32::encode(vector.key);
        check(session.setTotp(id, uri.toStdString()), "set RFC test vector with padded URI key");
        const auto code = session.totpCode(id);
        check(code.status == Status::Ready && code.value == vector.expected && code.startsAtMs == 30000
              && code.expiresAtMs == 60000, "RFC 6238 Appendix B output and shared clock interval");
    }
    TestClock::at(60000);
    const auto rotated = session.totpCode(id);
    check(rotated.status == Status::Ready && rotated.value != vectors[2].expected
          && rotated.startsAtMs == 60000 && rotated.expiresAtMs == 90000, "code rotates at its validity boundary");
    check(session.setTotp(id, "jbsw y3dp ehpk 3pxp") && session.totpCode(id).value.size() == 6,
          "manual key is normalized and uses the KeePassXC defaults");
    check(session.setTotp(id, "mzxw6===") && session.totpCode(id).status == Status::Ready,
          "padded Base32 manual key survives KeePassXC serialization");
    check(session.setTotp(id, "otpauth://totp/Steam?secret=JBSWY3DPEHPK3PXP&encoder=steam&digits=5&period=60"),
          "upstream Steam encoder and custom interval");
    const auto steam = session.totpCode(id);
    check(steam.value.size() == 5 && steam.expiresAtMs == 120000, "Steam code length and period");

    for (const auto input : {"bad!key", "A", "otpauth://hotp/Example?secret=JBSWY3DPEHPK3PXP&counter=0",
            "otpauth://totp/Example?secret=JBSWY3DPEHPK3PXP&period=0",
            "otpauth://totp/Example?secret=JBSWY3DPEHPK3PXP&digits=10",
            "otpauth://totp/Example?secret=JBSWY3DPEHPK3PXP&digits=9&encoder=steam",
            "otpauth://totp/Example?secret=JBSWY3DPEHPK3PXP&algorithm=MD5",
            "otpauth://totp/Example?secret=JBSWY3DPEHPK3PXP&secret=MZXW6"}) {
        check(!session.setTotp(id, input) && session.totpCode(id).value == steam.value,
              "invalid setup preserves the previous code");
    }
    TestClock::restore();
}

void checkSavedAttributes(const QString& path)
{
    Database db;
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QString::fromLatin1(CoreSession::FixturePassword)));
    KeePass2Reader reader;
    check(reader.readDatabase(path, key, &db), "upstream can read the saved KDBX");
    const auto* replaced = db.rootGroup()->findEntryByPath(QStringLiteral("合成 Legacy"));
    const auto* removed = db.rootGroup()->findEntryByPath(QStringLiteral("合成 KeePass 2"));
    check(replaced && replaced->attributes()->isProtected(Totp::ATTRIBUTE_OTP)
          && replaced->attributes()->value(Totp::ATTRIBUTE_OTP).contains("issuer=Example"), "protected URI retains issuer");
    // The fixture used Entry::setTotp for the legacy setup, which already created one history item.
    check(replaced->historyItems().size() == 2
          && replaced->historyItems().back()->attributes()->contains(Totp::ATTRIBUTE_SEED), "one history item added for replacement");
    check(removed && !removed->attributes()->contains(Totp::KP2_TOTP_SECRET) && removed->historyItems().size() == 1
          && removed->historyItems().front()->attributes()->contains(Totp::KP2_TOTP_SECRET), "KeePass 2 removal keeps history");
}

void checkWorker(const QString& worker)
{
    QTemporaryDir temp;
    check(temp.isValid(), "private synthetic fixture directory");
    const auto source = temp.filePath("synthetic.kdbx");
    const auto recovery = temp.filePath("recovery");
    check(CoreSession::createFixture(source.toStdString(), FixtureFormat::Kdbx41), "write synthetic formats");
    check(QDir().mkdir(recovery) && ::chmod(QFile::encodeName(recovery), 0700) == 0, "protect recovery directory");
    RemoteSession session(worker);
    check(session.unlock(source.toStdString(), CoreSession::FixturePassword) && session.enableRecovery(recovery.toStdString()),
          "unlock isolated worker with a protected checkpoint");
    const auto legacyEntries = session.search("Legacy");
    const auto keepass2Entries = session.search("KeePass 2");
    const auto uriEntries = session.search("OTP URI");
    check(legacyEntries.size() == 1 && keepass2Entries.size() == 1 && uriEntries.size() == 1, "locate synthetic entries");
    const auto& legacy = legacyEntries.front().id;
    const auto& keepass2 = keepass2Entries.front().id;
    const auto& uri = uriEntries.front().id;
    for (const auto& id : {uri, legacy, keepass2}) {
        const auto code = session.totpCode(id);
        check(code.status == Status::Ready && code.value.size() == 6 && code.expiresAtMs - code.startsAtMs == 30000,
              "existing URI, legacy and KeePass 2 formats through the worker");
    }
    const auto originalCheckpoint = session.lastCheckpoint();
    check(!session.setTotp(legacy, "invalid!") && session.lastCheckpoint() == originalCheckpoint && !session.isModified(),
          "rejected input does not commit a checkpoint");
    check(session.setTotp(legacy, std::string("otpauth://totp/Example:user?issuer=Example&secret=") + TestKey)
          && session.lastCheckpoint() != originalCheckpoint && session.isModified(), "setup captures protected recovery");
    const auto setupCheckpoint = session.lastCheckpoint();
    check(session.removeTotp(keepass2) && session.lastCheckpoint() != setupCheckpoint
          && session.totpCode(keepass2).status == Status::NotConfigured, "remove imported settings and checkpoint");
    EntryDetails details;
    check(session.entry(uri, details), "ordinary entry edit loads");
    details.notes = "edited";
    check(session.updateEntry(details) && session.totpCode(uri).status == Status::Ready, "ordinary edit retains TOTP");
    check(session.save(), "save TOTP changes into KDBX");
    session.lock();
    check(session.totpCode(legacy).status == Status::Unavailable && !session.setTotp(legacy, TestKey), "lock gates code and key");
    check(session.unlock(source.toStdString(), CoreSession::FixturePassword), "reopen saved DB");
    check(session.totpCode(legacy).status == Status::Ready && session.totpCode(keepass2).status == Status::NotConfigured,
          "replacement and removal persist across reopen");
    check(session.removeEntry(legacy) && session.totpCode(legacy).status == Status::Unavailable, "recycled entries are gated");
    checkSavedAttributes(source);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    check(argc == 2 && CoreSession::initialize(), "initialize TOTP test");
    checkKnownCodes();
    checkWorker(QString::fromLocal8Bit(argv[1]));
    std::puts("PASS TOTP RFC vectors, clock, compatibility, private worker, checkpoint and lock");
}
