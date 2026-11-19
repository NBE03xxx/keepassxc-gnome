#pragma once
// Synthetic fixtures only. Included in a separate pristine-core reference
// executable as well as the GUI probe; never linked into the product.
#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryAttributes.h"
#include "core/Group.h"
#include "core/Totp.h"
#include "format/KeePass2.h"
#include "format/KeePass2Reader.h"
#include "format/KeePass2Writer.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"
#include "keys/FileKey.h"
#include <QBuffer>
#include <QFile>
#include <cstdio>
#include <cstdlib>

namespace KeyFileFixture {
inline constexpr auto Master = "synthetic-keyfile-master";
inline constexpr auto Original = "Key file fixture";
inline void check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr,"FAIL key file: %s\n",label); std::exit(2); }
}
inline QByteArray keyBytes(int format) {
    QByteArray bytes;
    if (format == 0) {
        QBuffer buffer(&bytes); check(buffer.open(QIODevice::WriteOnly),"XML v2 buffer"); FileKey::createXMLv2(&buffer);
    } else if (format == 1) {
        bytes = "<KeyFile><Meta><Version>1.00</Version></Meta><Key><Data>" + QByteArray(32,'k').toBase64() + "</Data></Key></KeyFile>";
    } else if (format == 2) bytes = QByteArray(32,'b');
    else if (format == 3) bytes = QByteArray(32,'h').toHex();
    else bytes = QByteArray("Synthetic arbitrary key file\n") + QByteArray(97,'a');
    return bytes;
}
inline void write(const QString& path, const QByteArray& bytes) {
    QFile file(path); check(file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(bytes)==bytes.size(),"write synthetic file");
}
inline QSharedPointer<CompositeKey> key(const QString& path, const QString& mode) {
    auto result=QSharedPointer<CompositeKey>::create();
    if (mode!="none") result->addKey(QSharedPointer<PasswordKey>::create(mode=="empty" ? QString{} : QString::fromLatin1(Master)));
    auto file=QSharedPointer<FileKey>::create(); check(file->load(path),"pristine upstream loads synthetic key"); result->addKey(file);
    return result;
}
inline void create(const QString& path, const QString& keyPath, const QString& mode, quint32 version) {
    Database db; check(db.setKey(key(keyPath,mode)),"set original composite key");
    auto kdf=KeePass2::uuidToKdf(version<KeePass2::FILE_VERSION_4 ? KeePass2::KDF_AES_KDBX3 : KeePass2::KDF_AES_KDBX4);
    kdf->setRounds(1000); check(db.changeKdf(kdf),"fast synthetic KDF"); db.setFormatVersion(version);
    auto* entry=new Entry; entry->setUuid(QUuid::createUuid()); entry->setGroup(db.rootGroup());
    entry->setTitle(Original); entry->setUsername("synthetic-key-user"); entry->setPassword("synthetic-entry-secret");
    entry->attributes()->set("Protected synthetic field","preserved",true);
    entry->attachments()->set("synthetic.txt","preserved attachment");
    entry->setTotp(Totp::createSettings("JBSWY3DPEHPK3PXP"));
    entry->beginUpdate(); entry->setNotes("preserved notes"); entry->endUpdate();
    QFile file(path); check(file.open(QIODevice::WriteOnly),"open synthetic KDBX output"); KeePass2Writer writer;
    check(writer.writeDatabase(&file,&db) && !writer.hasError(),"pristine core writes key-file KDBX");
}
inline void verify(const QString& path, const QString& keyPath, const QString& mode, const QString& title) {
    QFile file(path); check(file.open(QIODevice::ReadOnly),"open roundtrip output"); Database db; KeePass2Reader reader;
    check(reader.readDatabase(&file,key(keyPath,mode),&db),"pristine core unlocks saved composite key");
    const auto entries=db.rootGroup()->entriesRecursive(); check(entries.size()==1,"one preserved entry"); const auto* e=entries[0];
    check(e->title()==title && e->username()=="synthetic-key-user" && e->password()=="synthetic-entry-secret","entry preserved");
    check(e->attributes()->value("Protected synthetic field")=="preserved" && e->attributes()->isProtected("Protected synthetic field"),"protected attribute preserved");
    check(e->attachments()->value("synthetic.txt")=="preserved attachment" && e->notes()=="preserved notes" && e->hasTotp(),"attachment notes and TOTP preserved");
    check(!e->historyItems().isEmpty(),"history preserved");
}
}
