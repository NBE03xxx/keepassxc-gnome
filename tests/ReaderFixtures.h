#pragma once
// Synthetic, key-known fault injection. All cryptography and format parsing
// use KeePassXC; this generator is never linked into the application.
#include "CoreSession.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "crypto/CryptoHash.h"
#include "crypto/SymmetricCipher.h"
#include "crypto/kdf/Kdf.h"
#include "crypto/kdf/Argon2Kdf.h"
#include "core/Endian.h"
#include "gnome/ReadBudget.h"
#include "format/Kdbx4Reader.h"
#include "format/Kdbx4Writer.h"
#include "format/Kdbx3Reader.h"
#include "format/Kdbx3Writer.h"
#include "format/KeePass2.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"
#include "streams/HmacBlockStream.h"
#include "streams/HashedBlockStream.h"
#include "streams/qtiocompressor.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <functional>
#include <sys/stat.h>
#include <sys/resource.h>

namespace ReaderTest {
inline constexpr auto Master = "synthetic-finalization-master";
inline constexpr auto Secret = "synthetic-finalization-entry";
inline unsigned cases = 0;
inline unsigned failures = 0;
inline bool observe = false;

inline void require(bool ok, const char* operation)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL fixture setup: %s\n", operation);
        std::exit(2);
    }
}

inline QSharedPointer<CompositeKey> key()
{
    auto result = QSharedPointer<CompositeKey>::create();
    result->addKey(QSharedPointer<PasswordKey>::create(QString::fromLatin1(Master)));
    return result;
}

class FixtureHeader final : public Kdbx4Reader
{
public:
    QByteArray seed() const { return m_masterSeed; }
    QByteArray iv() const { return m_encryptionIV; }
};

class LegacyHeader final : public Kdbx3Reader
{
public:
    QByteArray seed() const { return m_masterSeed; }
    QByteArray iv() const { return m_encryptionIV; }
    QByteArray start() const { return m_streamStartBytes; }
};

struct Fixture {
    QByteArray prefix;
    QByteArray plaintext;
    QByteArray finalKey;
    QByteArray hmacKey;
    QByteArray iv;
    SymmetricCipher::Mode mode;
    bool compressed;
    std::string entry;
    bool legacy = false;
    QByteArray start;
};

inline Fixture makeFixture(quint32 version, const QUuid& cipher, bool compressed,
                    const QUuid& kdfUuid = KeePass2::KDF_AES_KDBX4,
                    const std::function<void(Database&, Entry&)>& decorate = {})
{
    Database db;
    auto kdf = KeePass2::uuidToKdf(kdfUuid);
    kdf->setRounds(1000); // Fast, synthetic fixtures only; never a product default.
    if (auto argon = qSharedPointerDynamicCast<Argon2Kdf>(kdf)) {
        argon->setMemory(32);
        argon->setParallelism(1);
        argon->setRounds(1);
    }
    require(db.changeKdf(kdf) && db.setKey(key()), "set fixture key");
    db.setCipher(cipher);
    db.setCompressionAlgorithm(compressed ? Database::CompressionGZip : Database::CompressionNone);
    db.setFormatVersion(version);
    auto* entry = new Entry;
    entry->setUuid(QUuid::createUuid());
    entry->setTitle(QStringLiteral("Synthetic finalization entry"));
    entry->setPassword(QString::fromLatin1(Secret));
    entry->setGroup(db.rootGroup());
    if (decorate) decorate(db, *entry);
    db.setFormatVersion(version);
    QByteArray bytes;
    QBuffer output(&bytes);
    require(output.open(QIODevice::WriteOnly), "fixture buffer");
    Kdbx4Writer writer;
    require(writer.writeDatabase(&output, &db), "upstream fixture writer");

    QBuffer input(&bytes);
    require(input.open(QIODevice::ReadOnly), "fixture input");
    FixtureHeader header;
    Database parameters;
    require(header.readDatabase(&input, {}, &parameters), "upstream header parser");
    const auto payload = input.pos() + 64;
    require(parameters.setKey(key(), false, false), "fixture transformed key");
    Fixture result;
    result.prefix = bytes.left(payload);
    result.finalKey = CryptoHash::hash(header.seed() + parameters.transformedDatabaseKey(), CryptoHash::Sha256);
    result.hmacKey = KeePass2::hmacKey(header.seed(), parameters.transformedDatabaseKey());
    result.iv = header.iv();
    result.mode = SymmetricCipher::cipherUuidToMode(cipher);
    result.compressed = compressed;
    result.entry = entry->uuid().toString().toStdString();
    require(input.seek(payload), "fixture payload offset");
    HmacBlockStream blocks(&input, result.hmacKey);
    require(blocks.open(QIODevice::ReadOnly), "fixture HMAC reader");
    QByteArray encrypted = blocks.readAll();
    require(blocks.atEnd() && input.atEnd(), "complete original HMAC stream");
    SymmetricCipher decrypt;
    require(decrypt.init(result.mode, SymmetricCipher::Decrypt, result.finalKey, result.iv)
            && decrypt.finish(encrypted), "fixture decryption and padding");
    if (compressed) {
        QBuffer packed(&encrypted);
        require(packed.open(QIODevice::ReadOnly), "fixture compressed input");
        QtIOCompressor gzip(&packed);
        gzip.setStreamFormat(QtIOCompressor::GzipFormat);
        require(gzip.open(QIODevice::ReadOnly), "fixture gzip reader");
        result.plaintext = gzip.readAll();
    } else {
        result.plaintext = encrypted;
    }
    require(result.plaintext.contains("</KeePassFile>"), "fixture XML root present");
    return result;
}

inline Fixture makeLegacyFixture(const QUuid& cipher, bool compressed,
                                const std::function<void(Database&, Entry&)>& decorate = {})
{
    Database db;
    auto kdf = KeePass2::uuidToKdf(KeePass2::KDF_AES_KDBX3);
    kdf->setRounds(1000);
    require(db.changeKdf(kdf) && db.setKey(key()), "legacy fixture key");
    db.setCipher(cipher);
    db.setCompressionAlgorithm(compressed ? Database::CompressionGZip : Database::CompressionNone);
    db.setFormatVersion(KeePass2::FILE_VERSION_3_1);
    auto* entry = new Entry;
    entry->setUuid(QUuid::createUuid());
    entry->setTitle(QStringLiteral("Synthetic legacy finalization entry"));
    entry->setPassword(QString::fromLatin1(Secret));
    entry->setGroup(db.rootGroup());
    if (decorate) decorate(db, *entry);
    db.setFormatVersion(KeePass2::FILE_VERSION_3_1);
    QByteArray bytes;
    QBuffer output(&bytes);
    require(output.open(QIODevice::WriteOnly), "legacy fixture buffer");
    Kdbx3Writer writer;
    require(writer.writeDatabase(&output, &db), "upstream legacy writer");
    QBuffer input(&bytes);
    require(input.open(QIODevice::ReadOnly), "legacy fixture input");
    LegacyHeader header;
    Database parameters;
    require(header.readDatabase(&input, {}, &parameters) && parameters.setKey(key(), false, false),
            "upstream legacy header and transformed key");
    Fixture result;
    result.legacy = true;
    result.prefix = bytes.left(input.pos());
    result.finalKey = CryptoHash::hash(header.seed() + parameters.transformedDatabaseKey(), CryptoHash::Sha256);
    result.iv = header.iv();
    result.start = header.start();
    result.mode = SymmetricCipher::cipherUuidToMode(cipher);
    result.compressed = compressed;
    result.entry = entry->uuid().toString().toStdString();
    auto decrypted = input.readAll();
    SymmetricCipher decrypt;
    require(decrypt.init(result.mode, SymmetricCipher::Decrypt, result.finalKey, result.iv)
            && decrypt.finish(decrypted) && decrypted.startsWith(result.start), "legacy fixture decryption");
    decrypted.remove(0, result.start.size());
    QBuffer framed(&decrypted);
    require(framed.open(QIODevice::ReadOnly), "legacy hashed input");
    HashedBlockStream blocks(&framed);
    require(blocks.open(QIODevice::ReadOnly), "legacy hash reader");
    auto encoded = blocks.readAll();
    require(blocks.atEnd() && framed.atEnd(), "complete legacy hash stream");
    if (compressed) {
        QBuffer packed(&encoded);
        require(packed.open(QIODevice::ReadOnly), "legacy gzip input");
        QtIOCompressor gzip(&packed);
        gzip.setStreamFormat(QtIOCompressor::GzipFormat);
        require(gzip.open(QIODevice::ReadOnly), "legacy gzip reader");
        result.plaintext = gzip.readAll();
    } else {
        result.plaintext = encoded;
    }
    require(result.plaintext.contains("</KeePassFile>"), "legacy fixture XML root");
    return result;
}

inline QByteArray compress(const Fixture& fixture, const QByteArray& plaintext)
{
    if (!fixture.compressed) return plaintext;
    QByteArray result;
    QBuffer output(&result);
    require(output.open(QIODevice::WriteOnly), "compression buffer");
    QtIOCompressor gzip(&output);
    gzip.setStreamFormat(QtIOCompressor::GzipFormat);
    require(gzip.open(QIODevice::WriteOnly) && gzip.write(plaintext) == plaintext.size(), "upstream compression");
    gzip.close();
    return result;
}

inline QByteArray authenticate(const Fixture& fixture, const QByteArray& encrypted)
{
    QByteArray result;
    QBuffer output(&result);
    require(output.open(QIODevice::WriteOnly), "HMAC output");
    // Deliberately cross cipher block boundaries with authenticated block edges.
    HmacBlockStream blocks(&output, fixture.hmacKey, 4093);
    require(blocks.open(QIODevice::WriteOnly) && blocks.write(encrypted) == encrypted.size()
            && blocks.reset(), "upstream HMAC writer");
    return fixture.prefix + result;
}

inline QByteArray rawCiphertext(const Fixture& fixture, QByteArray encoded, bool badPadding = false)
{
    SymmetricCipher cipher;
    require(cipher.init(fixture.mode, SymmetricCipher::Encrypt, fixture.finalKey, fixture.iv), "fixture encryption init");
    if (badPadding) {
        const auto padding = 16 - encoded.size() % 16;
        encoded.append(QByteArray(padding, static_cast<char>(padding)));
        encoded[encoded.size() - 1] = 0; // Key-known, authenticated invalid padding.
        require(cipher.process(encoded), "fixture raw CBC encryption");
    } else {
        require(cipher.finish(encoded), "fixture encryption finish");
    }
    return encoded;
}

inline QByteArray hashLegacy(const QByteArray& encoded)
{
    QByteArray result;
    QBuffer output(&result);
    require(output.open(QIODevice::WriteOnly), "legacy hash output");
    HashedBlockStream blocks(&output, 4093);
    require(blocks.open(QIODevice::WriteOnly) && blocks.write(encoded) == encoded.size()
            && blocks.reset(), "upstream legacy hash writer");
    return result;
}

inline QByteArray encrypt(const Fixture& fixture, const QByteArray& encoded, bool badPadding = false)
{
    if (fixture.legacy) {
        return fixture.prefix + rawCiphertext(fixture, fixture.start + hashLegacy(encoded), badPadding);
    }
    return authenticate(fixture, rawCiphertext(fixture, encoded, badPadding));
}

inline void check(const QTemporaryDir& directory, const Fixture& fixture, const QByteArray& bytes,
           const char* name, bool expected, const ReadLimits& limits = {},
           ReadFailure failure = ReadFailure::None, int kdfStarts = -1, unsigned entryCount = 1)
{
    ++cases;
    const auto path = directory.filePath(QStringLiteral("input.kdbx"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(bytes) == bytes.size(), "write private synthetic input");
    file.close();
    CoreSession session;
    const bool accepted = session.unlock(path.toStdString(), Master, limits);
    bool correct = accepted == expected;
    const auto stats = session.lastReadStats();
    correct = correct && stats.groupObjects <= limits.groupObjects && stats.entryObjects <= limits.entryObjects
        && stats.historyItems <= limits.historyItems && stats.modelItems <= limits.modelItems
        && stats.xmlElements <= limits.xmlElements && stats.maximumXmlDepth <= limits.xmlDepth;
    if (failure != ReadFailure::None) correct = correct && session.lastReadFailure() == failure;
    if (kdfStarts >= 0) correct = correct && session.lastReadStats().kdfStarts == static_cast<unsigned>(kdfStarts);
    correct = correct && GnomeRead::current == nullptr;
    if (accepted && expected) {
        correct = correct && session.lastReadFailure() == ReadFailure::None
            && session.entries().size() == entryCount && session.copyPassword(fixture.entry) == Secret;
    } else if (!accepted) {
        correct = correct && session.entries().empty() && session.groups().empty()
            && session.copyPassword(fixture.entry).empty()
            && !session.saveCopy(directory.filePath(QStringLiteral("must-not-save.kdbx")).toStdString())
            && !QFile::exists(directory.filePath(QStringLiteral("must-not-save.kdbx")));
    }
    QFile unchanged(path);
    correct = correct && unchanged.open(QIODevice::ReadOnly)
        && CryptoHash::hash(unchanged.readAll(), CryptoHash::Sha256) == CryptoHash::hash(bytes, CryptoHash::Sha256);
    if (!correct) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: expected %s, got %s; reason=%d, KDF starts=%llu\n", name,
                     expected ? "accept" : "reject", accepted ? "accept" : "reject",
                     static_cast<int>(session.lastReadFailure()), static_cast<unsigned long long>(session.lastReadStats().kdfStarts));
    } else if (observe) {
        std::printf("PASS %s (%s)\n", name, accepted ? "accept" : "reject");
    }
}

struct HeaderFields {
    quint32 version;
    std::vector<std::pair<char, QByteArray>> fields;
    QByteArray tail;
    quint64 size;
};

inline HeaderFields fields(const QByteArray& bytes)
{
    QBuffer input;
    input.setData(bytes);
    require(input.open(QIODevice::ReadOnly), "synthetic header input");
    HeaderFields result{};
    quint32 first, second;
    require(KdbxReader::readMagicNumbers(&input, first, second, result.version), "upstream magic parser");
    for (unsigned count = 0; count < 64; ++count) {
        const auto id = input.read(1);
        bool ok;
        const auto length = result.version < KeePass2::FILE_VERSION_4
            ? Endian::readSizedInt<quint16>(&input, KeePass2::BYTEORDER, &ok)
            : Endian::readSizedInt<quint32>(&input, KeePass2::BYTEORDER, &ok);
        require(id.size() == 1 && ok && length <= input.bytesAvailable(), "small generated header field");
        result.fields.push_back({id[0], input.read(length)});
        if (!id[0]) {
            result.size = input.pos();
            result.tail = input.readAll();
            return result;
        }
    }
    require(false, "generated header terminator");
    return result;
}

class HeaderWriter : public Kdbx4Writer {
public:
    using KdbxWriter::writeHeaderField;
};

inline QByteArray encodeHeader(const HeaderFields& header)
{
    QByteArray result;
    QBuffer output(&result);
    require(output.open(QIODevice::WriteOnly), "synthetic header output");
    HeaderWriter writer;
    require(writer.writeMagicNumbers(&output, KeePass2::SIGNATURE_1, KeePass2::SIGNATURE_2, header.version), "upstream magic writer");
    for (const auto& [id, data] : header.fields) {
        const auto field = static_cast<KeePass2::HeaderFieldID>(id);
        require(header.version < KeePass2::FILE_VERSION_4
                    ? writer.writeHeaderField<quint16>(&output, field, data)
                    : writer.writeHeaderField<quint32>(&output, field, data), "upstream header field writer");
    }
    return result + header.tail;
}

inline QByteArray serializedMap(const QVariantMap& map)
{
    // Exercise the real private serializer through the public custom-data path.
    Database db;
    auto kdf = KeePass2::uuidToKdf(KeePass2::KDF_AES_KDBX4);
    kdf->setRounds(1000);
    require(db.changeKdf(kdf) && db.setKey(key()), "dictionary fixture key");
    db.setPublicCustomData(map);
    db.setFormatVersion(KeePass2::FILE_VERSION_4_1);
    QByteArray result;
    QBuffer output(&result);
    require(output.open(QIODevice::WriteOnly), "dictionary fixture output");
    Kdbx4Writer writer;
    require(writer.writeDatabase(&output, &db), "upstream dictionary serializer");
    for (const auto& [id, data] : fields(result).fields) {
        if (id == static_cast<char>(KeePass2::HeaderFieldID::PublicCustomData)) return data;
    }
    require(false, "serialized dictionary present");
    return {};
}

}
