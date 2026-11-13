#include "CoreSession.h"
#ifdef GNOME_PROCESS_TEST
#include "gnome/RemoteSession.h"
using TestedSession = RemoteSession;
#else
using TestedSession = CoreSession;
#endif

#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "format/KeePass2Reader.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    check(CoreSession::initialize(), "upstream crypto initialization");
#ifdef GNOME_PROCESS_TEST
    // The package/VM smoke test must exercise the explicitly supplied installed
    // helper, rather than silently using the sibling from the development tree.
    TestedSession session(argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString{});
#else
    TestedSession session;
#endif
    check(!session.create("Synthetic", ""), "empty creation password refused");
    check(!session.create("", "synthetic-phase3-password"), "empty database name refused");
    check(session.create("合成 DB", "synthetic-phase3-password"), "create database");
    const auto root = session.groups().front().id;
    check(!session.removeGroup(root), "root cannot be recycled");
    const auto group = session.addGroup(root, "Accounts");
    check(!group.empty() && session.groups().size() == 2, "group creation");
    check(session.renameGroup(group, "アカウント"), "group rename");
    EntryDetails details{{}, group, "Synthetic account", "synthetic-user", "synthetic-secret",
                         "https://example.invalid", "synthetic notes"};
    const auto entry = session.addEntry(group, details);
    check(!entry.empty() && session.entries().size() == 1, "entry creation");
    check(session.search("title:Synthetic", group).size() == 1, "upstream search syntax");
    check(session.search("missing").empty(), "nonmatching search");
    check(session.entry(entry, details) && details.password == "synthetic-secret", "entry details");
    check(session.copyUsername(entry) == "synthetic-user" && session.copyPassword(entry) == "synthetic-secret",
          "copy plain credentials");
    check(session.copyUsername("invalid").empty() && session.copyPassword("invalid").empty(), "unknown copy target refused");
    details.title = "変更済み";
    details.username = "updated-user";
    check(session.updateEntry(details), "entry edit");
    QTemporaryDir temp;
    check(temp.isValid(), "private test directory");
    const auto saved = temp.filePath("synthetic.kdbx");
    check(session.saveCopy(saved.toStdString()), "save separate copy");
    check(!session.saveCopy(saved.toStdString()), "existing destination protected");
    session.lock();
    check(session.entries().empty() && !session.entry(entry, details), "lock hides entry contents");
    check(!session.unlock(saved.toStdString(), "wrong"), "wrong password refused");
    check(session.unlock(saved.toStdString(), "synthetic-phase3-password"), "reopen saved database");
    check(session.entry(entry, details) && details.title == "変更済み" && details.username == "updated-user"
              && details.notes == "synthetic notes", "edited fields survived reopen");

    Database db;
    KeePass2Reader reader;
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QStringLiteral("synthetic-phase3-password")));
    check(reader.readDatabase(saved, key, &db), "independent upstream read");
    const auto* item = db.rootGroup()->findEntryByUuid(QUuid(QString::fromStdString(entry)));
    check(item && item->historyItems().size() == 1 && item->historyItems().front()->title() == "Synthetic account",
          "edit retained history");
    check(session.removeEntry(entry) && session.entries().empty(), "entry recycling");
    check(!session.entry(entry, details), "recycled entry hidden");
    check(session.copyUsername(entry).empty() && session.copyPassword(entry).empty(), "recycled entry cannot be copied");
    check(session.removeGroup(group) && session.groups().size() == 1, "group recycling");
    check(CoreSession::generatePassword(32).size() == 32, "existing password generator");
    check(CoreSession::generatePassword(1).empty(), "generator length bounds");
    session.lock();
    check(session.copyUsername(entry).empty() && session.copyPassword(entry).empty(), "lock revokes copy operations");
    std::puts("PASS database, groups, entries, search, history, generator, save and lock");
}
