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
#include "core/Metadata.h"
#include "format/KeePass2Reader.h"
#include "format/KeePass2Writer.h"
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
    check(session.groups().back().icon == 48 && !session.groups().back().customIcon, "default group folder icon");
    check(session.setGroupIcon(group, 66), "set standard group icon");
    check(!session.setGroupIcon(group, -1) && !session.setGroupIcon(group, 69) && !session.setGroupIcon("invalid", 0), "invalid group icons refused");
    check(session.updateGroup(group, "編集後のグループ", 25), "edit group name and icon together");
    check(!session.updateGroup(group, "must not apply", 69) && !session.updateGroup(group, "", 19), "invalid combined group edits refused");
    check(session.groups().back().name == "編集後のグループ" && session.groups().back().icon == 25, "invalid edit never partially renames or changes group icon");
    check(session.updateGroup(group, "アカウント", 66), "set final combined group edit");
    EntryDetails details{{}, group, "Synthetic account", "synthetic-user", "synthetic-secret",
                         "https://example.invalid", "synthetic notes"};
    const auto entry = session.addEntry(group, details);
    check(!entry.empty() && session.entries().size() == 1, "entry creation");
    check(session.entries().front().icon == 0 && !session.entries().front().customIcon, "default entry key icon");
    check(session.search("", root).empty() && session.search("", group).size() == 1, "group list shows direct entries; global entries remain recursive");
    check(session.search("title:Synthetic", group).size() == 1, "upstream search syntax");
    check(session.search("missing").empty(), "nonmatching search");
    check(session.entry(entry, details) && details.password == "synthetic-secret", "entry details");
    check(session.copyUsername(entry) == "synthetic-user" && session.copyPassword(entry) == "synthetic-secret",
          "copy plain credentials");
    check(session.copyUsername("invalid").empty() && session.copyPassword("invalid").empty(), "unknown copy target refused");
    details.title = "変更済み";
    details.username = "updated-user";
    details.icon = 19;
    check(session.updateEntry(details), "entry edit");
    details.icon = 69; check(!session.updateEntry(details), "invalid entry icon refused"); details.icon = 19;
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
              && details.notes == "synthetic notes" && details.icon == 19 && !details.customIcon, "edited fields and icon survived reopen");
    check(session.groups().back().icon == 66, "group icon survived reopen");

    Database db;
    KeePass2Reader reader;
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QStringLiteral("synthetic-phase3-password")));
    check(reader.readDatabase(saved, key, &db), "independent upstream read");
    const auto* item = db.rootGroup()->findEntryByUuid(QUuid(QString::fromStdString(entry)));
    check(item && item->historyItems().size() == 1 && item->historyItems().front()->title() == "Synthetic account",
          "edit retained history");
    check(item->iconNumber() == 19 && item->group()->iconNumber() == 66 && item->historyItems().front()->iconNumber() == 0,
          "upstream reads icon IDs and original history icon");
    check(session.removeEntry(entry) && session.entries().empty(), "entry recycling");
    check(!session.entry(entry, details), "recycled entry hidden");
    check(session.copyUsername(entry).empty() && session.copyPassword(entry).empty(), "recycled entry cannot be copied");
    check(session.removeGroup(group) && session.groups().size() == 1, "group recycling");
    check(CoreSession::generatePassword(32).size() == 32, "existing password generator");
    check(CoreSession::generatePassword(1).empty(), "generator length bounds");
    session.lock();
    check(session.copyUsername(entry).empty() && session.copyPassword(entry).empty(), "lock revokes copy operations");

    // A text-only edit must not silently replace a custom icon with IconID 0.
    const auto customId = QUuid::createUuid();
    const auto png = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");
    db.metadata()->addCustomIcon(customId, png, {}, {});
    auto* customEntry = db.rootGroup()->findEntryByUuid(QUuid(QString::fromStdString(entry)));
    customEntry->setIcon(customId); customEntry->group()->setIcon(customId);
    const auto customFile = temp.filePath("custom.kdbx");
    KeePass2Writer customWriter;
    check(customWriter.writeDatabase(customFile, &db), "write upstream custom icon fixture");
    check(session.unlock(customFile.toStdString(), "synthetic-phase3-password"), "open custom icon fixture");
    check(session.entry(entry, details) && details.customIcon && session.groups().back().customIcon, "custom icon metadata survives IPC");
    check(session.updateGroup(group, "Custom group retained"), "name-only group edit preserves custom icon");
    details.title = "Custom icon retained";
    check(session.updateEntry(details), "text edit preserves current custom icon");
    const auto preservedFile = temp.filePath("custom-preserved.kdbx");
    check(session.saveCopy(preservedFile.toStdString()), "save custom icon after text edit");
    Database preserved; KeePass2Reader preservedReader;
    check(preservedReader.readDatabase(preservedFile, key, &preserved), "upstream reads preserved custom icon");
    const auto* preservedEntry = preserved.rootGroup()->findEntryByUuid(customEntry->uuid());
    check(preservedEntry && preservedEntry->iconUuid() == customId && preservedEntry->group()->iconUuid() == customId
          && preserved.metadata()->customIcon(customId).data == png, "custom UUID and image bytes unchanged after edit and save");
    details.icon = 68; details.customIcon = false;
    check(session.updateEntry(details) && session.updateGroup(group, "Standard group", 48), "explicit choices replace custom icons");
    check(session.entry(entry, details) && !details.customIcon && details.icon == 68 && !session.groups().back().customIcon,
          "standard choices returned after custom replacement");
    session.lock();
    std::puts("PASS database, groups, entries, search, history, generator, save and lock");
}
