#include "AutoTypeSequence.h"
#include "gnome/RemoteSession.h"
#include "gnome/SessionProtocol.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "format/KeePass2Writer.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace
{
using Kind = AutoTypeStep::Kind;
using Status = AutoTypeSequence::Status;

void check(bool condition, const char* label)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
}

void rejected(const Entry& entry, Status status, const char* label)
{
    auto sequence = selectedEntryAutoType(entry);
    check(sequence.status == status && sequence.steps.empty(), label);
}

void checkCore()
{
    Database db;
    auto* entry = new Entry;
    entry->setGroup(db.rootGroup());
    entry->setUsername("u");
    entry->setPassword("p+%#");
    auto sequence = selectedEntryAutoType(*entry);
    check(sequence.status == Status::Ready && sequence.steps == std::vector<AutoTypeStep>{
              {Kind::Character, U'u'}, {Kind::Tab}, {Kind::Character, U'p'}, {Kind::Character, U'+'},
              {Kind::Character, U'%'}, {Kind::Character, U'#'}, {Kind::Enter}
          }, "upstream default and literal credentials");
    entry->setUsername({});
    sequence = selectedEntryAutoType(*entry);
    check(sequence.steps.size() == 5 && sequence.steps[0] == AutoTypeStep{Kind::Character, U'p'}
              && sequence.steps.back().kind == Kind::Enter, "upstream password-only default");

    db.rootGroup()->setDefaultAutoTypeSequence(QString::fromUtf8("é🧪{{}{+}{}}{dElAy 50}{Tab}{Password}"));
    sequence = selectedEntryAutoType(*entry);
    check(sequence.status == Status::Ready && sequence.steps.size() == 11
              && sequence.steps[0] == AutoTypeStep{Kind::Character, U'é'}
              && sequence.steps[1] == AutoTypeStep{Kind::Character, U'🧪'}
              && sequence.steps[2] == AutoTypeStep{Kind::Character, U'{'}
              && sequence.steps[3] == AutoTypeStep{Kind::Character, U'+'}
              && sequence.steps[4] == AutoTypeStep{Kind::Character, U'}'}
              && sequence.steps[5] == AutoTypeStep{Kind::Delay, 50}
              && sequence.steps[6].kind == Kind::Tab, "group inheritance, escaping, Unicode and bounded delay");

    entry->setDefaultAutoTypeSequence("{PASSWORD}");
    check(selectedEntryAutoType(*entry).steps.size() == 4, "entry override");
    entry->setAutoTypeEnabled(false);
    rejected(*entry, Status::Unavailable, "disabled entry");
    entry->setAutoTypeEnabled(true);
    db.rootGroup()->setAutoTypeEnabled(Group::Disable);
    rejected(*entry, Status::Unavailable, "disabled group");
    db.rootGroup()->setAutoTypeEnabled(Group::Inherit);

    for (const auto* syntax : {"{PASSWORD}{TOTP}", "{PASSWORD}{TAB 2}", "{PASSWORD}{DELAY 5001}",
                               "{PASSWORD}{DELAY 3000}{DELAY 2001}", "^{PASSWORD}", "{PASSWORD}{",
                               "{ENTER}{TAB}"}) {
        entry->setDefaultAutoTypeSequence(QString::fromLatin1(syntax));
        rejected(*entry, Status::Unsupported, "reject unsupported sequence without partial actions");
    }
    entry->setDefaultAutoTypeSequence("{PASSWORD}");
    entry->setPassword(QString(513, u'a'));
    rejected(*entry, Status::Unsupported, "step budget");
    entry->setPassword("line\nbreak");
    rejected(*entry, Status::Unsupported, "credential control character");
    entry->setPassword("valid");
    entry->setExpires(true);
    entry->setExpiryTime(QDateTime::currentDateTimeUtc().addDays(-1));
    rejected(*entry, Status::Unavailable, "expired entry");
}

void checkWorker(const QString& executable)
{
    RemoteSession session(executable);
    check(session.create("Synthetic", "only-for-test"), "create private synthetic database");
    auto group = session.groups().front().id;
    const auto id = session.addEntry(group, {{}, group, "Synthetic login", "u", "p", {}, {}});
    check(!id.empty(), "create entry");
    const auto sequence = session.autoTypeSequence(id);
    check(sequence.status == Status::Ready && sequence.steps == std::vector<AutoTypeStep>{
              {Kind::Character, U'u'}, {Kind::Tab}, {Kind::Character, U'p'}, {Kind::Enter}
          }, "typed actions through the private worker protocol");
    check(session.autoTypeSequence("missing").status == Status::Unavailable, "wrong entry id");
    session.lock();
    check(session.autoTypeSequence(id).status == Status::Unavailable, "locked worker refuses sequence");
}

void checkProtocol()
{
    for (const auto& steps : {
             QJsonArray{QJsonArray{int(Kind::Character), 0x110000}},
             QJsonArray{QJsonArray{int(Kind::Character), 10}},
             QJsonArray{QJsonArray{int(Kind::Character), 97}, QJsonArray{int(Kind::Tab), 2}},
             QJsonArray{QJsonArray{int(Kind::Character), 97}, QJsonArray{int(Kind::Delay), 5001}},
             QJsonArray{QJsonArray{int(Kind::Enter), 0}},
         }) {
        bool rejected = false;
        try {
            SessionProtocol::autoTypeSequence(QJsonArray{int(Status::Ready), steps});
        } catch (const SessionProtocol::Error&) {
            rejected = true;
        }
        check(rejected, "worker protocol refuses malformed key plans");
    }
}

void writeImeFixture(const QString& path)
{
    Database database;
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QString::fromLatin1(CoreSession::FixturePassword)));
    check(database.setKey(key), "set the synthetic fixture key");
    const std::pair<const char*, const char*> fixtures[]{
        {"Primary synthetic login", ""},
        {"Delayed synthetic login", "{USERNAME}{DELAY 4000}{TAB}{PASSWORD}{ENTER}"},
        {"Synthetic login without Enter", "{USERNAME}{TAB}{PASSWORD}"},
    };
    for (const auto& [title, sequence] : fixtures) {
        auto* entry = new Entry;
        entry->setUuid(QUuid::createUuid());
        entry->setGroup(database.rootGroup());
        entry->setTitle(QString::fromLatin1(title));
        entry->setUsername(QStringLiteral("synthetic-login-user"));
        entry->setPassword(QStringLiteral("synthetic-login-password"));
        entry->setDefaultAutoTypeSequence(QString::fromLatin1(sequence));
    }
    QFile output(path);
    check(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create the synthetic fixture exclusively");
    KeePass2Writer writer;
    check(writer.writeDatabase(&output, &database), "write the synthetic IME fixture");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    check(CoreSession::initialize(), "initialize test");
    if (argc == 3 && std::string_view(argv[1]) == "--write-ime-fixture") {
        writeImeFixture(QString::fromLocal8Bit(argv[2]));
        return 0;
    }
    check(argc == 2, "worker path required");
    checkCore();
    checkProtocol();
    checkWorker(QString::fromLocal8Bit(argv[1]));
    std::puts("PASS Auto-Type core, private worker and lock gate");
}
