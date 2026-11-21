// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnome/BrowserSession.h"
#include "gnome/BrowserAccess.h"
#include "gnome/BrowserNaming.h"
#include "gnome/CheckpointStore.h"

#include "browser/BrowserMessageBuilder.h"
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <optional>
#include <utility>
#include <variant>

namespace
{
const auto ChangeKeys = QStringLiteral("change-public-keys");
const auto DatabaseHash = QStringLiteral("get-databasehash");
const auto Associate = QStringLiteral("associate");
const auto TestAssociate = QStringLiteral("test-associate");
const auto GetLogins = QStringLiteral("get-logins");

QJsonObject reply(const QJsonObject& message)
{
    return {{"response", message}};
}

QJsonObject error(const QString& action, int code)
{
    return reply(browserMessageBuilder()->getErrorReply(action, code));
}

bool canonicalBase64(const QString& value, qsizetype size)
{
    if (value.size() != 4 * ((size + 2) / 3)) {
        return false;
    }
    const auto bytes = browserMessageBuilder()->base64Decode(value);
    return bytes.size() == size && value == QString::fromLatin1(bytes.toBase64());
}

bool includesPairing(const QJsonArray& keys, const QString& name, const QString& key)
{
    return std::any_of(keys.begin(), keys.end(), [&](const QJsonValue& value) {
        const auto item = value.toObject();
        return item.value("id").toString() == name && item.value("key").toString() == key;
    });
}
}

struct BrowserSession::Peer
{
    struct Association { QString key; };
    struct Login {
        QString origin;
        QString pairing;
        QByteArray requestId;
        QSet<QString> choices;
        quint64 revision;
    };
    struct Pending {
        QString token;
        QString hash;
        QString responseNonce;
        std::variant<Association, Login> request;
        QDeadlineTimer expires{60000};
    };

    ~Peer()
    {
        if (!secret.isEmpty()) {
            ::explicit_bzero(secret.data(), secret.size() * sizeof(QChar));
        }
    }

    QString clientKey;
    QString secret;
    QString pairing;
    QString pairingKey;
    QByteArray declinedLogin;
    QDeadlineTimer repeatWindow;
    QSet<QString> nonces;
    std::optional<Pending> pending;
};

BrowserSession::BrowserSession(CoreSession& database, CheckpointStore& checkpoints)
    : database(database), checkpoints(checkpoints)
{
}

BrowserSession::~BrowserSession() = default;

QJsonObject BrowserSession::request(int connection, const QJsonObject& message)
{
    const auto action = message.value("action").toString();
    if (action.size() > 64 || QJsonDocument(message).toJson(QJsonDocument::Compact).size() > MaxFrame) {
        return error({}, ERROR_KEEPASS_INCORRECT_ACTION);
    }
    if (action != ChangeKeys && action != DatabaseHash && action != Associate && action != TestAssociate
        && action != GetLogins) {
        return error(action, ERROR_KEEPASS_INCORRECT_ACTION);
    }
    const auto hash = database.browserDatabaseHash();
    if (connection <= 0 || hash.isEmpty() || !checkpoints.enabled()) {
        return error(action, ERROR_KEEPASS_DATABASE_NOT_OPENED);
    }
    auto* builder = browserMessageBuilder();
    const auto nonce = message.value("nonce").toString();
    if (!canonicalBase64(nonce, crypto_box_NONCEBYTES)) {
        return error(action, ERROR_KEEPASS_CANNOT_DECRYPT_MESSAGE);
    }
    const auto next = builder->incrementNonce(nonce);
    if (action == ChangeKeys) {
        const auto publicKey = message.value("publicKey").toString();
        if (!canonicalBase64(publicKey, crypto_box_PUBLICKEYBYTES)) {
            return error(action, ERROR_KEEPASS_CLIENT_PUBLIC_KEY_NOT_RECEIVED);
        }
        if (!peers.contains(connection) && peers.size() >= 4) {
            return error(action, ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED);
        }
        auto keys = builder->getKeyPair();
        auto peer = std::make_unique<Peer>();
        peer->clientKey = publicKey;
        peer->secret = std::move(keys.second);
        peers[connection] = std::move(peer);
        auto response = builder->buildMessage(next);
        response["action"] = action;
        response["publicKey"] = keys.first;
        return reply(response);
    }

    const auto iterator = peers.find(connection);
    if (iterator == peers.end()) {
        return error(action, ERROR_KEEPASS_CLIENT_PUBLIC_KEY_NOT_RECEIVED);
    }
    auto& peer = *iterator->second;
    if (peer.nonces.size() >= 4096) {
        close(connection);
        return error(action, ERROR_KEEPASS_CLIENT_PUBLIC_KEY_NOT_RECEIVED);
    }
    if (peer.nonces.contains(nonce) || peer.nonces.contains(next)) {
        return error(action, ERROR_KEEPASS_CANNOT_DECRYPT_MESSAGE);
    }
    const auto payload = builder->decryptMessage(message.value("message").toString(), nonce, peer.clientKey,
                                                  peer.secret, MaxFrame);
    if (payload.value("action").toString() != action) {
        return error(action, ERROR_KEEPASS_CANNOT_DECRYPT_MESSAGE);
    }
    peer.nonces.insert(nonce);
    peer.nonces.insert(next);
    if (action == DatabaseHash) {
        return reply(builder->buildResponse(action, next, {{"hash", hash}}, peer.clientKey, peer.secret));
    }

    const auto key = payload.value("key").toString();
    if (action == TestAssociate) {
        const auto name = payload.value("id").toString();
        if (!validBrowserConnectionName(name) || !canonicalBase64(key, crypto_box_PUBLICKEYBYTES)
            || !database.matchesBrowserAssociation(name, key)) {
            peer.pairing.clear();
            peer.pairingKey.clear();
            return error(action, ERROR_KEEPASS_ASSOCIATION_FAILED);
        }
        peer.pairing = name;
        peer.pairingKey = key;
        return reply(builder->buildResponse(action, next, {{"hash", hash}, {"id", name}}, peer.clientKey, peer.secret));
    }

    for (auto& [id, other] : peers) {
        if (other->pending && other->pending->expires.hasExpired()) {
            other->pending.reset();
        }
        if (other->pending) {
            return error(action, ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED);
        }
    }

    const auto token = QUuid::createUuid().toString(QUuid::Id128);
    if (action == GetLogins) {
        if (peer.pairing.isEmpty() || payload.value("id").toString() != peer.pairing
            || !database.matchesBrowserAssociation(peer.pairing, peer.pairingKey)
            || !includesPairing(payload.value("keys").toArray(), peer.pairing, peer.pairingKey)) {
            return error(action, ERROR_KEEPASS_ASSOCIATION_FAILED);
        }
        const auto httpAuth = payload.value("httpAuth");
        const auto site = payload.value("url").toString();
        const auto destination = payload.value("submitUrl").toString();
        const auto origin = browserHttpsOrigin(site);
        if ((!httpAuth.isUndefined() && httpAuth.toString() != u"false")
            || origin.isEmpty() || origin != browserHttpsOrigin(destination)) {
            return error(action, ERROR_KEEPASS_NO_LOGINS_FOUND);
        }
        const auto requestId = QCryptographicHash::hash(site.toUtf8() + '\0' + destination.toUtf8(),
                                                        QCryptographicHash::Sha256);
        // The official manual fill action can request the credentials a
        // second time as soon as the first request is denied.
        if (!peer.repeatWindow.hasExpired() && peer.declinedLogin == requestId) {
            return error(action, ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED);
        }
        const auto choices = database.browserLoginChoices(origin, BrowserLoginLimit);
        if (choices.isEmpty() || choices.size() > BrowserLoginLimit) {
            return error(action, ERROR_KEEPASS_NO_LOGINS_FOUND);
        }
        QSet<QString> ids;
        for (const auto& choice : choices) ids.insert(choice.toObject().value("id").toString());
        peer.pending.emplace(Peer::Pending{token, hash, next,
            Peer::Login{origin, peer.pairing, requestId, std::move(ids), revision}});
        return {{"approval", token}, {"origin", origin}, {"choices", choices}};
    }

    const auto idKey = payload.value("idKey").toString();
    if (key != peer.clientKey || !canonicalBase64(idKey, crypto_box_PUBLICKEYBYTES) || idKey == key) {
        return error(action, ERROR_KEEPASS_ASSOCIATION_FAILED);
    }
    peer.pending.emplace(Peer::Pending{token, hash, next, Peer::Association{idKey}});
    return {{"approval", peer.pending->token}};
}

QJsonObject BrowserSession::decide(int connection, const QString& token, const QString& selection)
{
    const auto iterator = peers.find(connection);
    if (iterator == peers.end() || !iterator->second->pending || iterator->second->pending->token != token) {
        return {};
    }
    auto& peer = *iterator->second;
    const auto pending = std::exchange(peer.pending, std::nullopt);
    const auto* login = std::get_if<Peer::Login>(&pending->request);
    const auto& action = login ? GetLogins : Associate;
    if (login && selection.isEmpty()) {
        peer.declinedLogin = login->requestId;
        peer.repeatWindow.setRemainingTime(2000);
    }
    if (pending->expires.hasExpired() || pending->hash != database.browserDatabaseHash()
        || selection.isEmpty() || !checkpoints.enabled()) {
        return error(action, ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED);
    }
    auto* builder = browserMessageBuilder();
    if (login) {
        if (login->revision != revision || peer.pairing != login->pairing
            || !database.matchesBrowserAssociation(peer.pairing, peer.pairingKey)
            || !login->choices.contains(selection)) {
            return error(action, ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED);
        }
        const auto entry = database.browserLogin(login->origin, selection);
        if (entry.isEmpty()) return error(action, ERROR_KEEPASS_NO_LOGINS_FOUND);
        const auto response = builder->buildResponse(action, pending->responseNonce,
            {{"count", 1}, {"entries", QJsonArray{entry}}, {"hash", pending->hash}, {"id", login->pairing}},
            peer.clientKey, peer.secret);
        if (QJsonDocument(response).toJson(QJsonDocument::Compact).size() > MaxFrame) {
            return error(action, ERROR_KEEPASS_CANNOT_ENCRYPT_MESSAGE);
        }
        return reply(response);
    }
    if (!validBrowserConnectionName(selection)) return error(action, ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED);
    auto response = builder->buildResponse(action, pending->responseNonce,
        {{"hash", pending->hash}, {"id", selection}}, peer.clientKey, peer.secret);
    if (!response.contains("message")) {
        return reply(response);
    }
    const auto& association = std::get<Peer::Association>(pending->request);
    if (!database.addBrowserAssociation(selection, association.key)) {
        return error(Associate, ERROR_KEEPASS_ASSOCIATION_FAILED);
    }
    // As with entry edits, a failed checkpoint terminates the worker before it can
    // report success or retain a model with unprotected changes.
    if (!checkpoints.commit(database)) {
        _exit(76);
    }
    peer.pairing = selection;
    peer.pairingKey = association.key;
    return {{"response", response}, {"registered", true}};
}

void BrowserSession::close(int connection)
{
    peers.erase(connection);
}
