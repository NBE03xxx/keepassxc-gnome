// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoreSession.h"
#include "gnome/BrowserAccess.h"

#include "core/CustomData.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntryPlaceholders.h"
#include "core/Group.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>

QString browserHttpsOrigin(const QString& input)
{
    if (input.isEmpty() || input.size() > 8192 || input.contains(u'\\')
        || std::any_of(input.cbegin(), input.cend(), [](QChar ch) {
               return ch.isSpace() || ch.category() == QChar::Other_Control;
           })) {
        return {};
    }
    const QUrl url(input, QUrl::StrictMode);
    const auto host = url.host(QUrl::FullyEncoded).toLower();
    const auto port = url.port(443);
    if (!url.isValid() || url.scheme() != u"https" || host.isEmpty() || host.endsWith(u'.')
        || url.authority(QUrl::FullyEncoded).contains(u'@') || port <= 0 || port > 65535) {
        return {};
    }
    QUrl origin;
    origin.setScheme(QStringLiteral("https"));
    origin.setHost(host);
    if (port != 443) origin.setPort(port);
    return origin.isValid() ? origin.toString(QUrl::FullyEncoded) : QString{};
}

namespace
{
const auto Hidden = QStringLiteral("BrowserHideEntry");
const auto OnlyHttpAuth = QStringLiteral("BrowserOnlyHttpAuth");
const auto RestrictKey = QStringLiteral("BrowserRestrictKey");
const auto AccessSettings = QStringLiteral("KeePassXC-Browser Settings");

bool restrictedFlag(const CustomData& data, const QString& key)
{
    return data.contains(key) && data.value(key) != u"false";
}

QString hostSetting(const QString& value)
{
    if (value.isEmpty() || value.contains(u'/') || value.contains(u'@')
        || value.contains(u'?') || value.contains(u'#') || value.contains(u'%')) {
        return {};
    }
    // The upstream settings contain hosts rather than URLs or ports.
    const auto authority = value.contains(u':') && !value.startsWith(u'[')
        ? u'[' + value + u']' : value;
    const auto origin = browserHttpsOrigin(QStringLiteral("https://") + authority);
    const QUrl url(origin);
    return origin.isEmpty() || url.port() != -1 ? QString{} : url.host(QUrl::FullyEncoded);
}

bool permitsHost(const Entry& entry, const QString& host)
{
    if (!entry.customData()->contains(AccessSettings)) return true;
    const auto document = QJsonDocument::fromJson(entry.customData()->value(AccessSettings).toUtf8());
    if (!document.isObject()) return false;
    const auto settings = document.object();
    bool allowListed = false;
    bool allowMatch = false;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (it.key() == u"objectName" || it.key() == u"Realm") {
            if (!it.value().isString() || (it.key() == u"Realm" && !it.value().toString().isEmpty())) return false;
            continue;
        }
        const bool allow = it.key() == u"Allow";
        if ((!allow && it.key() != u"Deny") || !it.value().isArray()) return false;
        const auto hosts = it.value().toArray();
        allowListed |= allow && !hosts.isEmpty();
        for (const auto& value : hosts) {
            const auto setting = hostSetting(value.toString());
            if (setting.isEmpty() || (!allow && setting == host)) return false;
            allowMatch |= allow && setting == host;
        }
    }
    return !allowListed || allowMatch;
}

bool eligible(const Entry& entry, const QString& origin, const QString& host)
{
    const auto* group = entry.group();
    if (!group || entry.isRecycled() || entry.isExpired() || !group->resolveSearchingEnabled()
        || browserHttpsOrigin(entry.url()) != origin) {
        return false;
    }
    const auto& data = *entry.customData();
    if (restrictedFlag(data, Hidden) || restrictedFlag(data, OnlyHttpAuth) || !data.value(RestrictKey).isEmpty()) {
        return false;
    }
    // Inherit restrictions conservatively. Upstream-only pairing names
    // do not authorize a GNOME pairing with an identical name.
    for (auto* current = group; current; current = current->parentGroup()) {
        const auto& options = *current->customData();
        if (current->isRecycled() || restrictedFlag(options, Hidden) || restrictedFlag(options, OnlyHttpAuth)
            || !options.value(RestrictKey).isEmpty()) {
            return false;
        }
    }
    const auto user = entry.username();
    const auto password = entry.password();
    return !password.isEmpty() && user.size() <= 4096 && password.size() <= 4096
        && !EntryPlaceholders::containsPlaceholder(user) && !EntryPlaceholders::containsPlaceholder(password)
        && permitsHost(entry, host);
}

QString displayLabel(const QString& text)
{
    auto label = text.left(120);
    for (auto& ch : label) {
        if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format
            || ch.category() == QChar::Separator_Line || ch.category() == QChar::Separator_Paragraph) {
            ch = u' ';
        }
    }
    label = label.simplified();
    if (text.size() > 120) label += u'…';
    return label;
}
}

QJsonArray CoreSession::browserLoginChoices(const QString& origin, int maximum) const
{
    QJsonArray choices;
    if (!database || !hasSource() || origin.isEmpty()) return choices;
    const auto host = QUrl(origin).host(QUrl::FullyEncoded);
    for (const auto* group : database->rootGroup()->groupsRecursive(true)) {
        for (const auto* entry : group->entries()) {
            if (!eligible(*entry, origin, host)) continue;
            choices.append(QJsonObject{{"id", entry->uuid().toString(QUuid::WithoutBraces)},
                                       {"title", displayLabel(entry->title())}, {"group", displayLabel(group->name())}});
            if (choices.size() > maximum) return choices;
        }
    }
    return choices;
}

QJsonObject CoreSession::browserLogin(const QString& origin, const QString& id) const
{
    if (!database || !hasSource() || origin.isEmpty()) return {};
    const auto* entry = findEntry(id.toStdString());
    if (!entry || !eligible(*entry, origin, QUrl(origin).host(QUrl::FullyEncoded))) return {};
    return {{"uuid", entry->uuidToHex()}, {"name", displayLabel(entry->title())},
            {"group", displayLabel(entry->group()->name())}, {"login", entry->username()},
            {"password", entry->password()}, {"skipAutoSubmit", "true"}};
}
