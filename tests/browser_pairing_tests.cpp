// SPDX-License-Identifier: GPL-3.0-or-later
#include "gnome/RemoteSession.h"
#include "gnome/BrowserAccess.h"
#include "browser/BrowserMessageBuilder.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/Group.h"
#include "format/KeePass2Writer.h"
#include "keys/CompositeKey.h"
#include "keys/PasswordKey.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUrl>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace
{
void check(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

QString nonce()
{
    QByteArray bytes(crypto_box_NONCEBYTES, '\0');
    randombytes_buf(bytes.data(), bytes.size());
    return QString::fromLatin1(bytes.toBase64());
}

struct Client
{
    explicit Client(int connection) : peer(connection) {}

    int peer;
    QPair<QString, QString> keys = browserMessageBuilder()->getKeyPair();
    QString server;

    void exchange(RemoteSession& session)
    {
        const auto challenge = nonce();
        const auto response = session.browserRequest(peer, {
            {"action", "change-public-keys"}, {"publicKey", keys.first}, {"nonce", challenge}
        });
        check(response.response.value("nonce").toString() == browserMessageBuilder()->incrementNonce(challenge),
              "browser key exchange");
        server = response.response.value("publicKey").toString();
    }

    QJsonObject request(const QString& action, QJsonObject payload = {}) const
    {
        const auto challenge = nonce();
        payload["action"] = action;
        return {{"action", action}, {"nonce", challenge},
                {"message", browserMessageBuilder()->encryptMessage(payload, challenge, server, keys.second)}};
    }

    QJsonObject open(const BrowserReply& reply) const
    {
        return browserMessageBuilder()->decryptMessage(reply.response.value("message").toString(),
            reply.response.value("nonce").toString(), server, keys.second);
    }
};

bool error(const BrowserReply& reply, int code)
{
    return reply.approval.isEmpty() && !reply.registered && !reply.response.contains("message")
        && reply.response.value("errorCode").toString() == QString::number(code);
}

void model(RemoteSession& session, const QString& path, const QString& recovery)
{
    check(session.unlock(path.toStdString(), CoreSession::FixturePassword), "open synthetic DB");
    check(QDir().mkdir(recovery) && ::chmod(QFile::encodeName(recovery), 0700) == 0, "private recovery folder");
    check(session.enableRecovery(recovery.toStdString()), "initial checkpoint");
}

void pairing(const QString& worker, const QString& base, FixtureFormat format)
{
    const auto source = base + ".kdbx";
    check(CoreSession::createFixture(source.toStdString(), format), "create synthetic DB");
    RemoteSession session(worker);
    check(session.browserRequest(1, {{"action", "get-databasehash"}}).response.isEmpty(), "locked worker gate");
    model(session, source, base + "-recovery");
    Client browser{1}, other{2};
    browser.exchange(session);
    other.exchange(session);
    const auto info = browser.request("get-databasehash");
    const auto hash = browser.open(session.browserRequest(browser.peer, info)).value("hash").toString();
    check(hash.size() == 64, "hash response decrypts");
    check(error(session.browserRequest(browser.peer, info), ERROR_KEEPASS_CANNOT_DECRYPT_MESSAGE), "nonce replay");
    check(error(session.browserRequest(other.peer, info), ERROR_KEEPASS_CANNOT_DECRYPT_MESSAGE), "peer isolation");

    const QString name = QStringLiteral("Chrome 検証");
    const auto idKey = browserMessageBuilder()->getKeyPair().first;
    const auto testRequest = [&] (const QString& key) {
        return browser.request("test-associate", {{"id", name}, {"key", key}});
    };
    auto begin = [&] {
        return session.browserRequest(browser.peer, browser.request("associate", {{"key", browser.keys.first}, {"idKey", idKey}}));
    };
    auto pending = begin();
    check(pending.response.isEmpty() && pending.approval.size() == 32, "only an approval token leaves the worker");
    const auto otherRequest = other.request("associate", {{"key", other.keys.first}, {"idKey", idKey}});
    check(error(session.browserRequest(other.peer, otherRequest), ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED),
          "only one approval may be pending");
    check(error(session.browserRequest(browser.peer, testRequest(idKey)), ERROR_KEEPASS_ASSOCIATION_FAILED), "no pairing before approval");
    check(session.browserDecision(other.peer, pending.approval, name).response.isEmpty(), "approval cannot cross peers");
    check(error(session.browserDecision(browser.peer, pending.approval), ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED), "explicit denial");
    check(!session.isModified(), "denied request does not modify model");
    check(session.browserDecision(browser.peer, pending.approval, name).response.isEmpty(), "consumed approval cannot be replayed");
    pending = begin();
    session.closeBrowserConnection(browser.peer);
    check(session.browserDecision(browser.peer, pending.approval, name).response.isEmpty(), "disconnect invalidates approval");
    browser.exchange(session);
    pending = begin();
    browser.exchange(session);
    check(session.browserDecision(browser.peer, pending.approval, name).response.isEmpty(), "key change invalidates approval");

    pending = begin();
    const auto before = session.lastCheckpoint();
    const auto approved = session.browserDecision(browser.peer, pending.approval, name);
    check(approved.registered && browser.open(approved).value("id").toString() == name
              && before != session.lastCheckpoint() && session.isModified(),
          "approval is returned after a new encrypted checkpoint");
    check(error(session.browserDecision(browser.peer, begin().approval, name), ERROR_KEEPASS_ASSOCIATION_FAILED),
          "duplicate name refused");
    check(error(session.browserRequest(browser.peer, testRequest(browser.keys.first)), ERROR_KEEPASS_ASSOCIATION_FAILED),
          "matching name with wrong identification key refused");
    check(browser.open(session.browserRequest(browser.peer, testRequest(idKey))).value("hash").toString() == hash,
          "matching identification key");
    check(error(session.browserRequest(browser.peer, browser.request("get-totp")), ERROR_KEEPASS_INCORRECT_ACTION),
          "pairing does not enable other browser actions");
    const auto recovered = base + "-checkpoint.kdbx";
    check(QFile::copy(QString::fromStdString(session.lastCheckpoint()), recovered), "copy encrypted checkpoint");
    check(session.save(), "save amended synthetic DB");
    pending = begin();
    session.lock();
    model(session, recovered, base + "-checkpoint-recovery");
    browser.exchange(session);
    check(session.browserDecision(browser.peer, pending.approval, name).response.isEmpty(), "lock invalidates approval");
    check(browser.open(session.browserRequest(browser.peer, testRequest(idKey))).value("hash").toString() == hash,
          "approved pairing persists in checkpoint");
    session.lock();
    model(session, source, base + "-reopened-recovery");
    browser.exchange(session);
    check(browser.open(session.browserRequest(browser.peer, testRequest(idKey))).value("hash").toString() == hash,
          "approved pairing survives save and reopen");

    pending = begin();
    const auto recovery = base + "-reopened-recovery";
    check(QDir().rename(recovery, recovery + "-moved"), "simulate inaccessible checkpoint root");
    const auto failed = session.browserDecision(browser.peer, pending.approval, "Another connection");
    check(failed.response.isEmpty() && !failed.registered && session.fatalFailure() == ReadFailure::RecoveryFailure,
          "checkpoint failure stops worker with no success response");
}

void loginFixture(const QString& file, const QString& origin)
{
    Database database;
    auto key = QSharedPointer<CompositeKey>::create();
    key->addKey(QSharedPointer<PasswordKey>::create(QString::fromLatin1(CoreSession::FixturePassword)));
    check(database.setKey(key), "synthetic fixture key");
    auto* root = database.rootGroup();
    root->setName(QStringLiteral("Synthetic logins"));
    const auto settings = QStringLiteral("KeePassXC-Browser Settings");
    auto add = [&](Group* group, const QString& title, const QString& url) {
        auto* entry = new Entry;
        entry->setUuid(QUuid::createUuid());
        entry->setGroup(group);
        entry->setTitle(title);
        entry->setUrl(url);
        entry->setUsername(QStringLiteral("synthetic-login-user"));
        entry->setPassword(QStringLiteral("synthetic-login-password"));
        return entry;
    };
    auto group = [&](const QString& title) {
        auto* result = new Group;
        result->setUuid(QUuid::createUuid());
        result->setName(title);
        result->setParent(root);
        return result;
    };
    const auto url = origin + QStringLiteral("/login");
    const auto host = QUrl(origin).host();
    auto* primary = add(root, "Primary synthetic login", url);
    primary->setNotes(QStringLiteral("private synthetic notes"));
    primary->customData()->set(settings, QStringLiteral("{\"Allow\":[\"%1\"],\"Realm\":\"\",\"objectName\":\"\"}").arg(host.toUpper()));
    add(root, "Alternate synthetic login", url)->setPassword(QStringLiteral("alternate-password"));
    add(root, "Wrong origin", "https://elsewhere.kpxc.test/login");
    add(root, "Expired", url)->expireNow();
    add(root, "Hidden", url)->customData()->set(QStringLiteral("BrowserHideEntry"), QStringLiteral("true"));
    add(root, "HttpAuth", url)->customData()->set(QStringLiteral("BrowserOnlyHttpAuth"), QStringLiteral("true"));
    add(root, "Denied host", url)->customData()->set(settings, QStringLiteral("{\"Allow\":[\"%1\"],\"Deny\":[\"%1\"]}").arg(host));
    add(root, "Allow list mismatch", url)->customData()->set(settings, QStringLiteral("{\"Allow\":[\"elsewhere.kpxc.test\"]}"));
    add(root, "Unknown settings", url)->customData()->set(settings, QStringLiteral("{\"Unrecognized\":true}"));
    add(root, "Realm", url)->customData()->set(settings, QStringLiteral("{\"Realm\":\"private\"}"));
    add(root, "Reference", url)->setPassword(QStringLiteral("{PASSWORD}"));
    auto* hidden = group(QStringLiteral("Hidden group"));
    hidden->customData()->set(QStringLiteral("BrowserHideEntry"), QStringLiteral("true"));
    add(hidden, "Group hidden", url);
    auto* restricted = group(QStringLiteral("Restricted group"));
    restricted->customData()->set(QStringLiteral("BrowserRestrictKey"), QStringLiteral("Chrome test"));
    add(restricted, "Group restricted", url);
    auto* notSearchable = group(QStringLiteral("Not searchable"));
    notSearchable->setSearchingEnabled(Group::Disable);
    add(notSearchable, "Search disabled", url);
    database.recycleEntry(add(root, "Recycled", url));
    QFile output(file);
    check(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), "new synthetic fixture");
    KeePass2Writer writer;
    check(writer.writeDatabase(&output, &database), "write synthetic login fixture");
}

void login(const QString& worker, const QString& base)
{
    const QString origin = "https://login.kpxc.test";
    const auto source = base + ".kdbx";
    loginFixture(source, origin);
    RemoteSession session(worker);
    model(session, source, base + "-recovery");
    Client browser{1}, other{2};
    browser.exchange(session);
    other.exchange(session);
    const QString pairing = "Chrome test";
    const auto key = browserMessageBuilder()->getKeyPair().first;
    const auto begin = [&](const Client& client, QJsonObject changes = {}) {
        QJsonObject payload{{"url", origin + "/login"}, {"submitUrl", origin + "/submit"},
                            {"id", pairing}, {"keys", QJsonArray{QJsonObject{{"id", pairing}, {"key", key}}}}};
        for (auto it = changes.begin(); it != changes.end(); ++it) payload[it.key()] = it.value();
        return session.browserRequest(client.peer, client.request("get-logins", payload));
    };
    check(error(begin(browser), ERROR_KEEPASS_ASSOCIATION_FAILED), "unpaired get-logins");
    auto association = session.browserRequest(browser.peer,
        browser.request("associate", {{"key", browser.keys.first}, {"idKey", key}}));
    check(session.browserDecision(browser.peer, association.approval, pairing).registered, "approve pairing");
    check(error(begin(other), ERROR_KEEPASS_ASSOCIATION_FAILED), "pairing cannot cross sockets");
    check(error(begin(browser, {{"keys", QJsonArray{}}}), ERROR_KEEPASS_ASSOCIATION_FAILED), "missing identification key");
    check(error(begin(browser, {{"id", "other"}}), ERROR_KEEPASS_ASSOCIATION_FAILED), "wrong pairing name");
    for (const auto& updates : {
            QJsonObject{{"url", "http://login.kpxc.test/login"}},
            QJsonObject{{"url", "https://login.kpxc.test.evil/login"}, {"submitUrl", "https://login.kpxc.test.evil/submit"}},
            QJsonObject{{"url", "https://other.login.kpxc.test/login"}, {"submitUrl", "https://other.login.kpxc.test/submit"}},
            QJsonObject{{"submitUrl", "https://login.kpxc.test:444/submit"}},
            QJsonObject{{"submitUrl", "https://other.kpxc.test/submit"}},
            QJsonObject{{"submitUrl", ""}},
            QJsonObject{{"url", "https://alice@login.kpxc.test/login"}},
            QJsonObject{{"httpAuth", "true"}},
        }) {
        check(error(begin(browser, updates), ERROR_KEEPASS_NO_LOGINS_FOUND), "unsafe or unsupported target refused");
    }

    auto pending = begin(browser, {{"submitUrl", "https://LOGIN.KPXC.TEST:443/submit"}});
    check(pending.approval.size() == 32 && pending.response.isEmpty() && pending.origin == origin,
          "only approval data leaves the worker");
    check(pending.choices.size() == 2 && pending.choices[0].title == u"Primary synthetic login"
        && pending.choices[1].title == u"Alternate synthetic login", "entry policy removes all restricted fixtures");
    const auto id = pending.choices[0].id;
    check(error(session.browserDecision(browser.peer, pending.approval), ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED), "deny login");
    check(session.browserDecision(browser.peer, pending.approval, id).response.isEmpty(), "cannot reuse approval");
    check(error(begin(browser, {{"submitUrl", "https://LOGIN.KPXC.TEST:443/submit"}}),
          ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED), "an immediate identical retry does not open a second dialog");
    pending = begin(browser);
    check(session.browserDecision(other.peer, pending.approval, id).response.isEmpty(), "login approval bound to peer");
    check(error(session.browserDecision(browser.peer, pending.approval, QUuid::createUuid().toString(QUuid::WithoutBraces)),
          ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED), "cannot choose an entry outside the displayed list");

    pending = begin(browser);
    check(session.rename(id.toStdString(), "Renamed during approval"), "fixture edit before decision");
    check(error(session.browserDecision(browser.peer, pending.approval, id), ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED),
          "model changes invalidate a prior approval");
    pending = begin(browser);
    const auto payload = browser.open(session.browserDecision(browser.peer, pending.approval, id));
    const auto entries = payload.value("entries").toArray();
    check(entries.size() == 1 && entries[0].toObject().value("login") == "synthetic-login-user"
        && entries[0].toObject().value("password") == "synthetic-login-password"
        && entries[0].toObject().value("skipAutoSubmit") == "true" && !entries[0].toObject().contains("totp"),
        "only selected credentials are returned encrypted, with auto-submit suppressed");
    pending = begin(browser);
    check(session.removeEntry(id.toStdString()), "fixture move to recycle bin");
    check(error(session.browserDecision(browser.peer, pending.approval, id), ERROR_KEEPASS_ACTION_CANCELLED_OR_DENIED),
          "eligibility rechecked after approval");
    pending = begin(browser);
    session.lock();
    check(session.browserDecision(browser.peer, pending.approval, pending.choices[0].id).response.isEmpty(),
          "locking invalidates pending credential release");
    check(browserHttpsOrigin("https://LOGIN.KPXC.TEST:443/login?x=1") == origin, "canonical HTTPS origin");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    check(CoreSession::initialize(), "initialize core");
    if (argc == 4 && std::string_view(argv[1]) == "--write-login-fixture") {
        loginFixture(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
        return 0;
    }
    QTemporaryDir vaults;
    check(argc == 2 && vaults.isValid(), "test setup");
    const auto worker = QString::fromLocal8Bit(argv[1]);
    for (const auto format : {FixtureFormat::Kdbx3, FixtureFormat::Kdbx4, FixtureFormat::Kdbx41}) {
        pairing(worker, vaults.filePath(QString::number(int(format))), format);
    }
    login(worker, vaults.filePath("logins"));
    std::puts("PASS: worker pairing, site and entry restrictions, approved fill response, lock and encrypted recovery");
}
