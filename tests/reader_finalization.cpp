#include "ReaderFixtures.h"

using namespace ReaderTest;

namespace {
void policyCases(const QTemporaryDir& directory)
{
    // Test-only process limits prevent a removed guard from allocating gigabytes
    // or spending unbounded time on deliberately excessive synthetic requests.
    struct rlimit memory{1ULL << 30, 1ULL << 30}, cpu{10, 10};
    require(setrlimit(RLIMIT_AS, &memory) == 0 && setrlimit(RLIMIT_CPU, &cpu) == 0, "test process safety limits");
    for (auto version : {KeePass2::FILE_VERSION_3_1, KeePass2::FILE_VERSION_4, KeePass2::FILE_VERSION_4_1}) {
        for (bool compressed : {false, true}) {
            const auto fixture = version < KeePass2::FILE_VERSION_4
                ? makeLegacyFixture(KeePass2::CIPHER_AES256, compressed)
                : makeFixture(version, KeePass2::CIPHER_AES256, compressed);
            const auto plain = fixture.plaintext + "\n<!--" + QByteArray(200000, 'x') + "-->\n";
            const auto encoded = compress(fixture, plain);
            const auto normal = encrypt(fixture, encoded);
            const auto header = fields(normal);
            auto run = [&](const char* name, const ReadLimits& limits, bool success, int starts) {
                check(directory, fixture, normal, name, success, limits,
                      success ? ReadFailure::None : ReadFailure::ResourceLimit, starts);
            };
            run("default policy control", {}, true, 1);
            ReadLimits limits;
            limits.fileBytes = normal.size(); run("file size at limit", limits, true, 1);
            --limits.fileBytes; run("file size beyond limit", limits, false, 0);
            limits = {}; limits.headerBytes = header.size; run("header bytes at limit", limits, true, 1);
            --limits.headerBytes; run("header bytes beyond limit", limits, false, 0);
            limits = {}; limits.headerFields = header.fields.size(); run("header count at limit", limits, true, 1);
            --limits.headerFields; run("header count beyond limit", limits, false, 0);
            quint64 largest = 0;
            for (const auto& item : header.fields) largest = std::max<quint64>(largest, item.second.size());
            limits = {}; limits.outerFieldBytes = largest; run("outer field at limit", limits, true, 1);
            --limits.outerFieldBytes; run("outer field beyond limit", limits, false, 0);
            limits = {}; limits.aesRounds = 1000; run("AES work at limit", limits, true, 1);
            --limits.aesRounds; run("AES work beyond limit", limits, false, 0);
            limits = {}; limits.blockBytes = compressed ? (fixture.legacy ? encoded.size() : rawCiphertext(fixture, encoded).size()) : 4093;
            run("block size at limit", limits, true, 1);
            --limits.blockBytes; run("block size beyond limit", limits, false, 1);
            limits = {}; limits.expandedBytes = !compressed && fixture.legacy
                ? fixture.start.size() + hashLegacy(encoded).size() : plain.size();
            run("decoded output at limit", limits, true, 1);
            --limits.expandedBytes; run("decoded output beyond limit", limits, false, 1);
            if (!fixture.legacy) {
                limits = {}; limits.dictionaryEntries = 3; run("dictionary count at limit", limits, true, 1);
                --limits.dictionaryEntries; run("dictionary count beyond limit", limits, false, 0);
                limits = {}; limits.dictionaryNameBytes = 5; run("dictionary name at limit", limits, true, 1);
                --limits.dictionaryNameBytes; run("dictionary name beyond limit", limits, false, 0);
                limits = {}; limits.innerFieldBytes = 64; run("inner field at limit", limits, true, 1);
                --limits.innerFieldBytes; run("inner field beyond limit", limits, false, 1);
                limits = {}; limits.innerFields = 3; run("inner count at limit", limits, true, 1);
                --limits.innerFields; run("inner count beyond limit", limits, false, 1);
                limits = {}; limits.innerHeaderBytes = 83; run("inner bytes at limit", limits, true, 1);
                --limits.innerHeaderBytes; run("inner bytes beyond limit", limits, false, 1);
                auto malformed = fixture.plaintext;
                malformed.replace(1, 4, Endian::sizedIntToBytes<quint32>(0x7fffffff, KeePass2::BYTEORDER));
                check(directory, fixture, encrypt(fixture, compress(fixture, malformed)), "excessive inner declaration", false, {}, ReadFailure::ResourceLimit, 1);
                auto badBlock = normal;
                badBlock.replace(fixture.prefix.size() + 32, 4,
                    Endian::sizedIntToBytes<quint32>(0x7fffffff, KeePass2::BYTEORDER));
                check(directory, fixture, badBlock, "excessive HMAC block declaration", false, {}, ReadFailure::ResourceLimit, 1);
            } else {
                for (auto rounds : {0ULL, 100000001ULL, (1ULL << 32) + 1000, 0xffffffffffffffffULL}) {
                    auto changed = header;
                    for (auto& [id, value] : changed.fields) {
                        if (id == static_cast<char>(KeePass2::HeaderFieldID::TransformRounds)) {
                            value = Endian::sizedIntToBytes<quint64>(rounds, KeePass2::BYTEORDER);
                        }
                    }
                    check(directory, fixture, encodeHeader(changed), "legacy AES raw rounds before narrowing", false, {},
                          rounds == 100000001ULL ? ReadFailure::ResourceLimit : ReadFailure::InvalidInput, 0);
                }
            }
            QByteArray shortHeader = normal.left(12);
            shortHeader += char(KeePass2::HeaderFieldID::MasterSeed);
            shortHeader += fixture.legacy ? Endian::sizedIntToBytes<quint16>(65535, KeePass2::BYTEORDER)
                                         : Endian::sizedIntToBytes<quint32>(0xffffffff, KeePass2::BYTEORDER);
            shortHeader += "tiny";
            check(directory, fixture, shortHeader, "excessive or truncated outer declaration", false, {},
                  fixture.legacy ? ReadFailure::InvalidInput : ReadFailure::ResourceLimit, 0);
        }
    }
    const auto fixture = makeFixture(KeePass2::FILE_VERSION_4_1, KeePass2::CIPHER_AES256, false);
    const auto normal = encrypt(fixture, fixture.plaintext);
    const auto original = fields(normal);
    auto dictionary = [&](const QByteArray& bytes, const char* name, ReadFailure failure) {
        auto changed = original;
        for (auto& [id, value] : changed.fields) {
            if (id == static_cast<char>(KeePass2::HeaderFieldID::KdfParameters)) value = bytes;
        }
        check(directory, fixture, encodeHeader(changed), name, false, {}, failure, 0);
    };
    QVariantMap aes{{QStringLiteral("$UUID"), KeePass2::KDF_AES_KDBX3.toRfc4122()},
                    {QStringLiteral("R"), QVariant::fromValue<quint64>(1000)},
                    {QStringLiteral("S"), QByteArray(32, 's')}};
    auto variant = aes;
    variant[QStringLiteral("R")] = QVariant::fromValue<quint64>(100000001);
    dictionary(serializedMap(variant), "excessive AES work", ReadFailure::ResourceLimit);
    for (auto value : {0ULL, 1ULL << 32, 0xffffffffffffffffULL}) {
        variant = aes; variant[QStringLiteral("R")] = QVariant::fromValue<quint64>(value);
        dictionary(serializedMap(variant), "invalid AES integer before narrowing", ReadFailure::InvalidInput);
    }
    variant = aes; variant[QStringLiteral("R")] = QStringLiteral("1000");
    dictionary(serializedMap(variant), "AES type coercion refused", ReadFailure::InvalidInput);
    variant = aes; variant[QStringLiteral("extra")] = QByteArray("keep me");
    dictionary(serializedMap(variant), "unknown KDF key refused", ReadFailure::UnsupportedKdf);
    const auto canonical = serializedMap(aes);
    auto damaged = canonical;
    damaged.replace(3, 4, Endian::sizedIntToBytes<quint32>(0x7fffffff, KeePass2::BYTEORDER));
    dictionary(damaged, "excessive dictionary name declaration", ReadFailure::ResourceLimit);
    damaged = canonical;
    damaged.replace(12, 4, Endian::sizedIntToBytes<quint32>(0x7fffffff, KeePass2::BYTEORDER));
    dictionary(damaged, "excessive dictionary value declaration", ReadFailure::ResourceLimit);
    damaged = canonical;
    damaged.replace(3, 4, Endian::sizedIntToBytes<quint32>(100, KeePass2::BYTEORDER));
    dictionary(damaged, "dictionary name beyond remaining bytes", ReadFailure::InvalidInput);
    damaged = canonical;
    damaged.replace(12, 4, Endian::sizedIntToBytes<quint32>(10000, KeePass2::BYTEORDER));
    dictionary(damaged, "dictionary value beyond remaining bytes", ReadFailure::InvalidInput);
    dictionary(canonical + "x", "dictionary trailing data", ReadFailure::InvalidInput);
    dictionary(canonical.left(canonical.size() - 1), "dictionary missing terminator", ReadFailure::InvalidInput);
    dictionary(canonical.left(canonical.size() - 1) + canonical.mid(2, 30) + QByteArray(1, 0), "duplicate dictionary key", ReadFailure::InvalidInput);
    for (const auto& uuid : {KeePass2::KDF_ARGON2D, KeePass2::KDF_ARGON2ID}) {
        const auto argonFixture = makeFixture(KeePass2::FILE_VERSION_4_1, KeePass2::CIPHER_AES256, true, uuid);
        check(directory, argonFixture, encrypt(argonFixture, compress(argonFixture, argonFixture.plaintext)),
              "Argon2 1.3 control", true, {}, ReadFailure::None, 1);
        const auto argonNormal = encrypt(argonFixture, compress(argonFixture, argonFixture.plaintext));
        ReadLimits budget;
        budget.argonMemoryKiB = 32; budget.argonRounds = 1; budget.argonLanes = 1; budget.argonWorkKiB = 32;
        check(directory, argonFixture, argonNormal, "Argon work at exact limits", true, budget, ReadFailure::None, 1);
        --budget.argonMemoryKiB;
        check(directory, argonFixture, argonNormal, "Argon memory above custom limit", false, budget, ReadFailure::ResourceLimit, 0);
        budget.argonMemoryKiB = 32; --budget.argonWorkKiB;
        check(directory, argonFixture, argonNormal, "Argon combined work above custom limit", false, budget, ReadFailure::ResourceLimit, 0);
        QVariantMap argon{{QStringLiteral("$UUID"), uuid.toRfc4122()}, {QStringLiteral("S"), QByteArray(32, 's')},
                          {QStringLiteral("V"), quint32(0x13)}, {QStringLiteral("P"), quint32(1)},
                          {QStringLiteral("M"), QVariant::fromValue<quint64>(32768)},
                          {QStringLiteral("I"), QVariant::fromValue<quint64>(1)}};
        variant = argon; variant[QStringLiteral("V")] = quint32(0x10);
        dictionary(serializedMap(variant), "Argon2 1.0 refused before KDF", ReadFailure::UnsupportedKdf);
        variant = argon; variant[QStringLiteral("M")] = QVariant::fromValue<quint64>(1ULL << 40);
        dictionary(serializedMap(variant), "excessive Argon memory", ReadFailure::ResourceLimit);
        variant = argon; variant[QStringLiteral("I")] = QVariant::fromValue<quint64>(21);
        dictionary(serializedMap(variant), "excessive Argon iterations", ReadFailure::ResourceLimit);
        variant = argon; variant[QStringLiteral("P")] = quint32(9); variant[QStringLiteral("M")] = QVariant::fromValue<quint64>(1024 * 128);
        dictionary(serializedMap(variant), "excessive Argon lanes", ReadFailure::ResourceLimit);
        variant = argon; variant[QStringLiteral("M")] = QVariant::fromValue<quint64>(256ULL << 20); variant[QStringLiteral("I")] = QVariant::fromValue<quint64>(5);
        dictionary(serializedMap(variant), "excessive combined Argon work", ReadFailure::ResourceLimit);
        variant = argon; variant[QStringLiteral("I")] = QVariant::fromValue<quint64>(1ULL << 32);
        dictionary(serializedMap(variant), "Argon integer narrowing refused", ReadFailure::InvalidInput);
        variant = argon; variant[QStringLiteral("M")] = QVariant::fromValue<quint64>(32769);
        dictionary(serializedMap(variant), "Argon memory rounding refused", ReadFailure::UnsupportedKdf);
        variant = argon; variant[QStringLiteral("V")] = QStringLiteral("19");
        dictionary(serializedMap(variant), "Argon type coercion refused", ReadFailure::InvalidInput);
        for (auto extra : {QStringLiteral("K"), QStringLiteral("A")}) {
            variant = argon; variant[extra] = QByteArray("synthetic");
            dictionary(serializedMap(variant), "unimplemented Argon input refused", ReadFailure::UnsupportedKdf);
        }
    }
    CoreSession session;
    ReadLimits invalid;
    ++invalid.fileBytes;
    check(directory, fixture, normal, "policy cannot raise the product ceiling", false, invalid, ReadFailure::InvalidInput, 0);
    const auto sparse = directory.filePath(QStringLiteral("oversized.kdbx"));
    QFile file(sparse);
    require(file.open(QIODevice::WriteOnly) && file.resize(ReadLimits{}.fileBytes + 1), "sparse oversized fixture");
    file.close();
    ++cases;
    if (session.unlock(sparse.toStdString(), Master) || session.lastReadFailure() != ReadFailure::ResourceLimit
        || session.lastReadStats().kdfStarts != 0) ++failures;
    const auto fifo = directory.filePath(QStringLiteral("synthetic.fifo"));
    require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "private FIFO fixture");
    ++cases;
    if (session.unlock(fifo.toStdString(), Master) || session.lastReadFailure() != ReadFailure::IoError
        || session.lastReadStats().kdfStarts != 0) ++failures;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    observe = argc == 2 && std::string(argv[1]) == "--observe";
    require(CoreSession::initialize(), "crypto initialization");
    QTemporaryDir directory;
    require(directory.isValid(), "private temporary directory");
    if (argc == 2 && std::string(argv[1]) == "--policy") {
        policyCases(directory);
        std::printf("Reader resource policy: %u cases, %u failures\n", cases, failures);
        return failures ? 1 : 0;
    }
    for (auto version : {KeePass2::FILE_VERSION_4, KeePass2::FILE_VERSION_4_1}) {
        for (const auto& cipher : {KeePass2::CIPHER_AES256, KeePass2::CIPHER_TWOFISH, KeePass2::CIPHER_CHACHA20}) {
            for (bool compressed : {false, true}) {
                auto fixture = makeFixture(version, cipher, compressed);
                for (bool comment : {false, true}) {
                    auto plaintext = fixture.plaintext;
                    if (comment) plaintext += "\n<!--" + QByteArray(256 * 1024, 'x') + "-->\n";
                    const auto encoded = compress(fixture, plaintext);
                    const auto normal = encrypt(fixture, encoded);
                    check(directory, fixture, normal, "complete document", true);
                    auto damaged = normal;
                    damaged[damaged.size() - 36] ^= 1;
                    check(directory, fixture, damaged, "terminal HMAC corruption", false);
                    check(directory, fixture, normal.left(normal.size() - 36), "missing terminal block", false);
                    check(directory, fixture, normal.left(normal.size() - 1), "short terminal block", false);
                    check(directory, fixture, normal + QByteArray(1, 'x'), "bytes after terminal block", false);
                    damaged = normal;
                    damaged[fixture.prefix.size()] ^= 1;
                    check(directory, fixture, damaged, "data HMAC corruption", false);
                    check(directory, fixture, encrypt(fixture, compress(fixture, plaintext + "<after/>")),
                          "second XML document element", false);
                    check(directory, fixture, encrypt(fixture, compress(fixture, plaintext + "<!--unfinished")),
                          "unfinished XML comment", false);
                    if (fixture.mode != SymmetricCipher::ChaCha20) {
                        check(directory, fixture, encrypt(fixture, encoded, true), "authenticated invalid CBC padding", false);
                        auto partial = rawCiphertext(fixture, encoded);
                        partial.chop(1);
                        check(directory, fixture, authenticate(fixture, partial), "authenticated partial CBC block", false);
                    }
                    if (compressed) {
                        damaged = encoded;
                        damaged[damaged.size() - 8] ^= 1;
                        check(directory, fixture, encrypt(fixture, damaged), "gzip CRC corruption", false);
                        check(directory, fixture, encrypt(fixture, encoded.left(encoded.size() - 8)), "missing gzip trailer", false);
                        check(directory, fixture, encrypt(fixture, encoded + "x"), "bytes after gzip member", false);
                    }
                }
            }
        }
    }
    for (const auto& cipher : {KeePass2::CIPHER_AES256, KeePass2::CIPHER_TWOFISH}) {
        for (bool compressed : {false, true}) {
            const auto fixture = makeLegacyFixture(cipher, compressed);
            for (bool comment : {false, true}) {
                auto plaintext = fixture.plaintext;
                if (comment) plaintext += "\n<!--" + QByteArray(256 * 1024, 'x') + "-->\n";
                const auto encoded = compress(fixture, plaintext);
                const auto normal = encrypt(fixture, encoded);
                const auto frames = hashLegacy(encoded);
                auto seal = [&fixture](const QByteArray& bytes) {
                    return fixture.prefix + rawCiphertext(fixture, fixture.start + bytes);
                };
                check(directory, fixture, normal, "legacy complete document", true);
                auto damaged = frames;
                damaged[damaged.size() - 36] ^= 1;
                check(directory, fixture, seal(damaged), "legacy terminal hash corruption", false);
                check(directory, fixture, seal(frames.left(frames.size() - 40)), "legacy missing terminal block", false);
                check(directory, fixture, seal(frames.left(frames.size() - 1)), "legacy short terminal block", false);
                check(directory, fixture, seal(frames + "x"), "legacy bytes after terminal block", false);
                check(directory, fixture, normal + "x", "legacy trailing file byte", false);
                check(directory, fixture, encrypt(fixture, encoded, true), "legacy invalid CBC padding", false);
                check(directory, fixture, normal.left(normal.size() - 1), "legacy partial CBC block", false);
                check(directory, fixture, encrypt(fixture, compress(fixture, plaintext + "<after/>")),
                      "legacy second XML element", false);
                check(directory, fixture, encrypt(fixture, compress(fixture, plaintext + "<!--unfinished")),
                      "legacy unfinished XML comment", false);
                if (compressed) {
                    damaged = encoded;
                    damaged[damaged.size() - 8] ^= 1;
                    check(directory, fixture, encrypt(fixture, damaged), "legacy gzip CRC corruption", false);
                    check(directory, fixture, encrypt(fixture, encoded.left(encoded.size() - 8)), "legacy missing gzip trailer", false);
                    check(directory, fixture, encrypt(fixture, encoded + "x"), "legacy bytes after gzip member", false);
                }
            }
        }
    }
    std::printf("Reader finalization: %u cases, %u failures%s\n", cases, failures,
                observe ? " (baseline observation only)" : "");
    return failures == 0 || observe ? 0 : 1;
}
