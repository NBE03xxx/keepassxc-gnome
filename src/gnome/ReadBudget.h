#pragma once

#include "ReadLimits.h"
#include "format/KeePass2.h"
#include <QMetaType>
#include <QSet>
#include <QVariantMap>
#include <algorithm>
#include <limits>

namespace GnomeRead {
struct Context {
    const ReadLimits& limits;
    ReadStats& stats;
    ReadFailure& failure;
    quint64 sourceBytes;
    quint64 headerBytes = 12;
    quint64 headerFields = 0;
    quint64 innerBytes = 0;
    quint64 innerFields = 0;
    bool countCipherOutput = false;
    quint32 sourceVersion = 0;
    QSet<quint8> outerIds{};
    QSet<quint8> innerIds{};
    QSet<QUuid> definedGroups{};
    QSet<QUuid> definedEntries{};
};
inline thread_local Context* current = nullptr;

class Scope {
public:
    explicit Scope(Context& context) : previous(current) { current = &context; }
    ~Scope() { current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Context* previous;
};

inline bool reject(ReadFailure reason)
{
    if (current && current->failure == ReadFailure::None) current->failure = reason;
    return false;
}

template<typename Used>
inline bool charge(Used& used, quint64 count, quint64 limit)
{
    if (used > limit || count > limit - used) return reject(ReadFailure::ResourceLimit);
    used += count;
    return true;
}

inline void beginHeader()
{
    if (current) { current->headerBytes = 12; current->headerFields = 0; current->outerIds.clear(); }
}

inline bool uniqueHeader(quint8 id, bool inner = false)
{
    if (!current) return true;
    auto& ids = inner ? current->innerIds : current->outerIds;
    if (inner && id == static_cast<quint8>(KeePass2::InnerHeaderFieldID::Binary)) return true;
    if (ids.contains(id)) return reject(ReadFailure::UnsupportedData);
    ids.insert(id);
    return true;
}

enum class Item { Group, Entry, Binary, Attachment, CustomData, Icon, Deleted, Association, String };

inline bool modelItem(Item item)
{
    if (!current) return true;
    auto& c = *current;
    if (c.failure != ReadFailure::None) return false;
    std::uint64_t *used = nullptr, limit = 0;
    switch (item) {
    case Item::Group: used = &c.stats.groupObjects; limit = c.limits.groupObjects; break;
    case Item::Entry: used = &c.stats.entryObjects; limit = c.limits.entryObjects; break;
    case Item::Binary: used = &c.stats.binaryItems; limit = c.limits.binaryItems; break;
    case Item::Attachment: used = &c.stats.attachmentReferences; limit = c.limits.attachmentReferences; break;
    case Item::CustomData: used = &c.stats.customDataItems; limit = c.limits.customDataItems; break;
    case Item::Icon: used = &c.stats.customIcons; limit = c.limits.customIcons; break;
    case Item::Deleted: used = &c.stats.deletedObjects; limit = c.limits.deletedObjects; break;
    case Item::Association: used = &c.stats.autoTypeAssociations; limit = c.limits.autoTypeAssociations; break;
    case Item::String: used = &c.stats.stringFields; limit = c.limits.stringFields; break;
    }
    if (*used >= limit || c.stats.modelItems >= c.limits.modelItems) return reject(ReadFailure::ResourceLimit);
    ++*used;
    ++c.stats.modelItems;
    return true;
}

inline bool historyItem()
{
    return !current || charge(current->stats.historyItems, 1, current->limits.historyItems);
}

inline bool definition(const QUuid& uuid, bool group)
{
    if (!current) return true;
    auto& definitions = group ? current->definedGroups : current->definedEntries;
    if (definitions.contains(uuid)) return reject(ReadFailure::UnsupportedData);
    definitions.insert(uuid);
    return true;
}

inline bool outerField(quint64 size, unsigned prefix)
{
    if (!current) return true;
    auto& c = *current;
    if (size > c.limits.outerFieldBytes || !charge(c.headerFields, 1, c.limits.headerFields)
        || !charge(c.headerBytes, prefix, c.limits.headerBytes)
        || !charge(c.headerBytes, size, c.limits.headerBytes)) return reject(ReadFailure::ResourceLimit);
    if (c.headerBytes > c.sourceBytes) return reject(ReadFailure::InvalidInput);
    c.stats.largestDeclaredRead = std::max<std::uint64_t>(c.stats.largestDeclaredRead, size);
    return true;
}

inline bool dictionaryLength(quint64 size, qint64 available, bool name)
{
    if (!current) return true;
    const auto limit = name ? current->limits.dictionaryNameBytes : current->limits.outerFieldBytes;
    if (size > limit) return reject(ReadFailure::ResourceLimit);
    if (available < 0 || size > static_cast<quint64>(available)) return reject(ReadFailure::InvalidInput);
    current->stats.largestDeclaredRead = std::max<std::uint64_t>(current->stats.largestDeclaredRead, size);
    return true;
}

inline bool innerField(quint64 size)
{
    if (!current) return true;
    auto& c = *current;
    if (size > c.limits.innerFieldBytes || !charge(c.innerFields, 1, c.limits.innerFields)
        || !charge(c.innerBytes, 5, c.limits.innerHeaderBytes)
        || !charge(c.innerBytes, size, c.limits.innerHeaderBytes)) return reject(ReadFailure::ResourceLimit);
    c.stats.largestDeclaredRead = std::max<std::uint64_t>(c.stats.largestDeclaredRead, size);
    return true;
}

inline bool block(quint64 size)
{
    if (!current) return true;
    if (size > current->limits.blockBytes) return reject(ReadFailure::ResourceLimit);
    current->stats.largestDeclaredRead = std::max<std::uint64_t>(current->stats.largestDeclaredRead, size);
    return true;
}

inline qint64 outputAllowance(qint64 requested)
{
    if (!current || requested <= 0) return requested;
    const auto used = current->stats.expandedBytes;
    const auto limit = current->limits.expandedBytes;
    const auto left = used < limit ? limit - used : 0;
    return static_cast<qint64>(std::min<quint64>(requested, left + 1));
}

inline bool consumeOutput(quint64 size)
{
    return !current || charge(current->stats.expandedBytes, size, current->limits.expandedBytes);
}

inline bool aesRounds(quint64 rounds)
{
    if (!current) return true;
    if (!rounds || rounds > std::numeric_limits<int>::max()) return reject(ReadFailure::InvalidInput);
    return rounds <= current->limits.aesRounds || reject(ReadFailure::ResourceLimit);
}

inline bool kdf(const QVariantMap& p)
{
    if (!current) return true;
    auto typed = [&p](const QString& name, int type) { return p.contains(name) && p.value(name).metaType().id() == type; };
    if (!typed(KeePass2::KDFPARAM_UUID, QMetaType::QByteArray)
        || p.value(KeePass2::KDFPARAM_UUID).toByteArray().size() != 16) return reject(ReadFailure::InvalidInput);
    const auto uuid = QUuid::fromRfc4122(p.value(KeePass2::KDFPARAM_UUID).toByteArray());
    const bool aes = uuid == KeePass2::KDF_AES_KDBX3 || uuid == KeePass2::KDF_AES_KDBX4;
    const bool argon = uuid == KeePass2::KDF_ARGON2D || uuid == KeePass2::KDF_ARGON2ID;
    if (!aes && !argon) return reject(ReadFailure::UnsupportedKdf);
    const QSet<QString> allowed = aes ? QSet<QString>{QStringLiteral("$UUID"), QStringLiteral("R"), QStringLiteral("S")}
        : QSet<QString>{QStringLiteral("$UUID"), QStringLiteral("V"), QStringLiteral("I"), QStringLiteral("M"),
                        QStringLiteral("P"), QStringLiteral("S")};
    for (auto it = p.cbegin(); it != p.cend(); ++it) {
        if (!allowed.contains(it.key())) return reject(ReadFailure::UnsupportedKdf);
    }
    if (!typed(QStringLiteral("S"), QMetaType::QByteArray)) return reject(ReadFailure::InvalidInput);
    const auto salt = p.value(QStringLiteral("S")).toByteArray().size();
    if (salt < 8 || salt > 32) return reject(ReadFailure::InvalidInput);
    if (aes) {
        if (salt != 32 || !typed(QStringLiteral("R"), QMetaType::ULongLong)) return reject(ReadFailure::InvalidInput);
        return aesRounds(p.value(QStringLiteral("R")).toULongLong());
    }
    if (!typed(QStringLiteral("V"), QMetaType::UInt) || !typed(QStringLiteral("P"), QMetaType::UInt)
        || !typed(QStringLiteral("M"), QMetaType::ULongLong) || !typed(QStringLiteral("I"), QMetaType::ULongLong))
        return reject(ReadFailure::InvalidInput);
    // Botan's current path implements v1.3; v1.0 must not silently use v1.3.
    if (p.value(QStringLiteral("V")).toUInt() != 0x13) return reject(ReadFailure::UnsupportedKdf);
    const auto lanes = p.value(QStringLiteral("P")).toUInt();
    const auto memoryBytes = p.value(QStringLiteral("M")).toULongLong();
    const auto rounds = p.value(QStringLiteral("I")).toULongLong();
    if (memoryBytes % 1024) return reject(ReadFailure::UnsupportedKdf);
    const auto memory = memoryBytes / 1024;
    if (!lanes || memory < 8ULL * lanes || !rounds || rounds > std::numeric_limits<int>::max())
        return reject(ReadFailure::InvalidInput);
    const auto& limits = current->limits;
    if (memory > limits.argonMemoryKiB || lanes > limits.argonLanes || rounds > limits.argonRounds
        || memory > limits.argonWorkKiB / rounds) return reject(ReadFailure::ResourceLimit);
    return true;
}
}
