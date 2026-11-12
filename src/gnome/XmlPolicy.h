#pragma once

#include "gnome/ReadBudget.h"
#include <QVector>
#include <QXmlStreamReader>

namespace GnomeRead {
// Qt still parses XML. These synchronous helper methods observe starts/ends
// before the KeePassXC parser can recurse or allocate a model object.
class XmlReader : public QXmlStreamReader
{
    struct Frame { QString name; QSet<QString> children; QSet<QString> keys; };
    QVector<Frame> stack;

    void fail(ReadFailure reason)
    {
        reject(reason);
        QXmlStreamReader::raiseError(QStringLiteral("XML exceeds the read policy or contains data that cannot be preserved."));
    }

    static bool repeats(const QString& parent, const QString& child)
    {
        return (parent == QStringLiteral("Group") && (child == QStringLiteral("Group") || child == QStringLiteral("Entry")))
            || (parent == QStringLiteral("Entry") && (child == QStringLiteral("String") || child == QStringLiteral("Binary")))
            || (parent == QStringLiteral("History") && child == QStringLiteral("Entry"))
            || (parent == QStringLiteral("CustomData") && child == QStringLiteral("Item"))
            || (parent == QStringLiteral("CustomIcons") && child == QStringLiteral("Icon"))
            || (parent == QStringLiteral("Binaries") && child == QStringLiteral("Binary"))
            || (parent == QStringLiteral("AutoType") && child == QStringLiteral("Association"))
            || (parent == QStringLiteral("DeletedObjects") && child == QStringLiteral("DeletedObject"));
    }

    bool attributesSupported(const QString& parent, const QString& node)
    {
        if (!namespaceUri().isEmpty() || !prefix().isEmpty() || !namespaceDeclarations().isEmpty()) return false;
        const auto attrs = attributes();
        for (const auto& attr : attrs) {
            if (!attr.namespaceUri().isEmpty() || !attr.prefix().isEmpty()) return false;
            const auto key = attr.name();
            const auto value = attr.value();
            const bool trueValue = value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 || value == QLatin1String("1");
            const bool falseValue = value.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0 || value == QLatin1String("0");
            if (parent == QStringLiteral("String") && node == QStringLiteral("Value")) {
                if ((key != QLatin1String("Protected") && key != QLatin1String("ProtectInMemory")) || (!trueValue && !falseValue)) return false;
            } else if (parent == QStringLiteral("Binaries") && node == QStringLiteral("Binary")) {
                if (key == QLatin1String("ID")) continue;
                if (key == QLatin1String("Compressed") && (trueValue || falseValue)) continue;
                if (key == QLatin1String("Protected") && falseValue) continue;
                return false;
            } else if (parent == QStringLiteral("Binary") && node == QStringLiteral("Value")) {
                if (key == QLatin1String("Ref")) continue;
                if (key == QLatin1String("Protected") && falseValue && !attrs.hasAttribute(QLatin1String("Ref"))) continue;
                return false;
            } else {
                return false;
            }
        }
        return true;
    }

public:
    void clear()
    {
        stack.clear();
        QXmlStreamReader::clear();
    }

    TokenType readNext()
    {
        const auto token = QXmlStreamReader::readNext();
        if (!current || hasError()) return token;
        if (token == StartElement) {
            if (stack.size() >= static_cast<qsizetype>(current->limits.xmlDepth)
                || !charge(current->stats.xmlElements, 1, current->limits.xmlElements)) {
                fail(ReadFailure::ResourceLimit);
                return Invalid;
            }
            const auto node = name().toString();
            const auto parent = stack.isEmpty() ? QString{} : stack.back().name;
            if (node == QStringLiteral("LastModificationTime") && parent == QStringLiteral("Item")
                && stack.size() >= 3 && stack[stack.size() - 2].name == QStringLiteral("CustomData")
                && (stack[stack.size() - 3].name != QStringLiteral("Meta")
                    || current->sourceVersion < KeePass2::FILE_VERSION_4_1)) {
                fail(ReadFailure::UnsupportedData);
                return Invalid;
            }
            if (!attributesSupported(parent, node)) { fail(ReadFailure::UnsupportedData); return Invalid; }
            if (!stack.isEmpty() && !repeats(parent, node)) {
                if (stack.back().children.contains(node)) { fail(ReadFailure::UnsupportedData); return Invalid; }
                stack.back().children.insert(node);
            }
            stack.push_back({node, {}, {}});
            current->stats.maximumXmlDepth = std::max<std::uint64_t>(current->stats.maximumXmlDepth, stack.size());
        } else if (token == EndElement) {
            if (!stack.isEmpty()) stack.pop_back();
        } else if (token == DTD || token == ProcessingInstruction) {
            fail(ReadFailure::UnsupportedData);
            return Invalid;
        }
        return token;
    }

    bool readNextStartElement()
    {
        if (!current) return QXmlStreamReader::readNextStartElement();
        while (!atEnd()) {
            const auto token = readNext();
            if (hasError()) return false;
            if (token == StartElement) return true;
            if (token == EndElement) return false;
            if (token == Characters && !isWhitespace()) { fail(ReadFailure::UnsupportedData); return false; }
            if (token == NoToken) { fail(ReadFailure::InvalidInput); return false; }
        }
        return false;
    }

    QString readElementText(ReadElementTextBehaviour behaviour = ErrorOnUnexpectedElement)
    {
        const auto node = current ? name().toString() : QString{};
        const auto result = QXmlStreamReader::readElementText(behaviour);
        if (current) {
            if (error() == UnexpectedElementError) fail(ReadFailure::UnsupportedData);
            if (!hasError() && node == QStringLiteral("Key") && stack.size() >= 3) {
                const auto& parent = stack[stack.size() - 2].name;
                auto& container = stack[stack.size() - 3];
                QString key;
                if (container.name == QStringLiteral("Entry") && parent == QStringLiteral("String")) key = QStringLiteral("S") + result;
                if (container.name == QStringLiteral("Entry") && parent == QStringLiteral("Binary")) key = QStringLiteral("B") + result;
                if (container.name == QStringLiteral("CustomData") && parent == QStringLiteral("Item")) key = QStringLiteral("I") + result;
                if (!key.isEmpty()) {
                    if (container.keys.contains(key)) fail(ReadFailure::UnsupportedData);
                    else container.keys.insert(key);
                }
            }
            if (isEndElement() && !stack.isEmpty()) stack.pop_back();
        }
        return result;
    }

    void skipCurrentElement()
    {
        if (!current) { QXmlStreamReader::skipCurrentElement(); return; }
        // The only supported direct skip is the empty Value with a binary Ref.
        if (name() != QLatin1String("Value") || !attributes().hasAttribute(QLatin1String("Ref"))) {
            fail(ReadFailure::UnsupportedData);
            return;
        }
        if (!readElementText().trimmed().isEmpty()) fail(ReadFailure::UnsupportedData);
    }
};
}
