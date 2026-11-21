#include "AutoTypeSequence.h"
#include "core/Entry.h"
#include "gnome/Sensitive.h"

#include <QStringView>

namespace
{
using Kind = AutoTypeStep::Kind;

bool append(AutoTypeSequence& plan, Kind kind, std::uint32_t value = 0)
{
    if (plan.steps.size() >= AutoTypeSequence::MaximumSteps) return false;
    plan.steps.push_back({kind, value});
    return true;
}

bool appendCharacter(AutoTypeSequence& plan, QStringView text, qsizetype& cursor)
{
    const auto first = text.at(cursor++);
    char32_t scalar = first.unicode();
    if (first.isHighSurrogate()) {
        if (cursor >= text.size() || !text.at(cursor).isLowSurrogate()) return false;
        scalar = QChar::surrogateToUcs4(first, text.at(cursor++));
    }
    // Field data can never add a Tab, Enter, or any other control action.
    return QChar::isPrint(scalar) && append(plan, Kind::Character, scalar);
}

bool appendField(AutoTypeSequence& plan, const Entry& entry, const QString& field)
{
    auto resolved = entry.resolveMultiplePlaceholders(field);
    if (resolved.isEmpty()) return true;
    const Sensitive::Erase clear{resolved.data(), std::size_t(resolved.size()) * sizeof(QChar)};
    for (qsizetype cursor = 0; cursor < resolved.size();) {
        if (!appendCharacter(plan, resolved, cursor)) return false;
    }
    return true;
}

bool parse(AutoTypeSequence& plan, const Entry& entry, QStringView sequence)
{
    unsigned totalDelay = 0;
    bool hasCharacter = false;
    for (qsizetype cursor = 0; cursor < sequence.size();) {
        const auto current = sequence.at(cursor);
        if (current != u'{') {
            // These characters have sequence syntax in KeePassXC. Reject an
            // unsupported modifier or brace instead of typing it literally.
            if (QStringView(u"}+^%#").contains(current) || !appendCharacter(plan, sequence, cursor)) return false;
            hasCharacter = true;
            continue;
        }

        const auto next = sequence.sliced(cursor);
        if (next.startsWith(u"{{}") || next.startsWith(u"{}}")) {
            if (!append(plan, Kind::Character, next.at(1).unicode())) return false;
            hasCharacter = true;
            cursor += 3;
            continue;
        }
        const auto end = sequence.indexOf(u'}', cursor + 1);
        if (end < 0) return false;
        const auto token = sequence.sliced(cursor + 1, end - cursor - 1);
        const auto equal = [token](QStringView name) { return token.compare(name, Qt::CaseInsensitive) == 0; };
        cursor = end + 1;

        const auto sizeBefore = plan.steps.size();
        if (equal(u"USERNAME")) {
            if (!appendField(plan, entry, entry.username())) return false;
            hasCharacter |= plan.steps.size() != sizeBefore;
        } else if (equal(u"PASSWORD")) {
            if (!appendField(plan, entry, entry.password())) return false;
            hasCharacter |= plan.steps.size() != sizeBefore;
        } else if (equal(u"TAB")) {
            if (!append(plan, Kind::Tab)) return false;
        } else if (equal(u"ENTER")) {
            if (!append(plan, Kind::Enter)) return false;
        } else if (equal(u"SPACE") || (token.size() == 1 && QStringView(u"+^%#").contains(token.at(0)))) {
            if (!append(plan, Kind::Character, equal(u"SPACE") ? U' ' : token.at(0).unicode())) return false;
            hasCharacter = true;
        } else if (token.startsWith(u"DELAY ", Qt::CaseInsensitive)) {
            const auto delay = token.sliced(6);
            if (delay.isEmpty() || delay.size() > 4) return false;
            unsigned ms = 0;
            for (const auto digit : delay) {
                if (digit < u'0' || digit > u'9') return false;
                ms = ms * 10 + unsigned(digit.unicode() - u'0');
            }
            totalDelay += ms;
            if (totalDelay > AutoTypeSequence::MaximumTotalDelayMs || !append(plan, Kind::Delay, ms)) return false;
        } else {
            return false;
        }
    }
    return hasCharacter;
}
}

AutoTypeSequence selectedEntryAutoType(const Entry& entry)
{
    AutoTypeSequence plan;
    if (entry.isRecycled() || entry.isExpired()) return plan;
    auto sequence = entry.effectiveAutoTypeSequence();
    if (sequence.isEmpty()) return plan;
    const Sensitive::Erase clear{sequence.data(), std::size_t(sequence.size()) * sizeof(QChar)};
    plan.steps.reserve(AutoTypeSequence::MaximumSteps);
    if (sequence.size() > 1024 || !parse(plan, entry, sequence)) {
        plan.clearSensitive();
        plan.status = AutoTypeSequence::Status::Unsupported;
        return plan;
    }
    plan.status = AutoTypeSequence::Status::Ready;
    return plan;
}
