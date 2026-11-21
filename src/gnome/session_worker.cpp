#include "CoreSession.h"
#include "gnome/BrowserSession.h"
#include "gnome/ProcessPolicy.h"
#include "gnome/SessionProtocol.h"
#include "gnome/SafeSave.h"
#include "gnome/CheckpointStore.h"
#include "gnome/Sensitive.h"
#include <QCoreApplication>
#include <QThreadPool>

using namespace SessionProtocol;

namespace {
QJsonValue dispatch(CoreSession& session, const QString& op, const QJsonArray& a)
{
    auto count = [&](int expected) { if (a.size() != expected) throw Error{}; };
    if (op == "unlock") {
        count(4); if (!a[1].isBool()) throw Error{};
        auto credentials = Sensitive::owned(new UnlockCredentials{a[1].toBool(), text(a[2]), text(a[3])});
        return session.unlock(text(a[0]), *credentials);
    }
    if (op == "create") {
        count(2); const auto secret = Sensitive::owned(new std::string(text(a[1])));
        return session.create(text(a[0]), *secret);
    }
    if (op == "groups") {
        count(0); QJsonArray rows;
        for (const auto& row : session.groups()) rows.append(QJsonArray{str(row.id), str(row.name), int(row.depth), row.icon, row.customIcon});
        return rows;
    }
    if (op == "search") {
        count(2); QJsonArray rows;
        for (const auto& row : session.search(text(a[0]), text(a[1])))
            rows.append(QJsonArray{str(row.id), str(row.title), str(row.username), str(row.group), row.icon, row.customIcon});
        return rows;
    }
    if (op == "entry") {
        count(1); auto value = Sensitive::owned(new EntryDetails);
        return session.entry(text(a[0]), *value) ? QJsonValue(encode(*value)) : QJsonValue(QJsonValue::Null);
    }
    if (op == "addGroup") { count(2); return str(session.addGroup(text(a[0]), text(a[1]))); }
    if (op == "renameGroup") { count(2); return session.renameGroup(text(a[0]), text(a[1])); }
    if (op == "updateGroup") { count(3); if (!a[2].isDouble()) throw Error{}; return session.updateGroup(text(a[0]), text(a[1]), a[2].toInt(-2)); }
    if (op == "setGroupIcon") { count(2); if (!a[1].isDouble()) throw Error{}; return session.setGroupIcon(text(a[0]), a[1].toInt(-1)); }
    if (op == "removeGroup") { count(1); return session.removeGroup(text(a[0])); }
    if (op == "addEntry") { count(2); auto value = Sensitive::owned(new EntryDetails(details(a[1]))); return str(session.addEntry(text(a[0]), *value)); }
    if (op == "updateEntry") { count(1); auto value = Sensitive::owned(new EntryDetails(details(a[0]))); return session.updateEntry(*value); }
    if (op == "removeEntry") { count(1); return session.removeEntry(text(a[0])); }
    if (op == "totpCode") {
        count(1);
        const auto value = Sensitive::owned(new TotpCode(session.totpCode(text(a[0]))));
        return encode(*value);
    }
    if (op == "setTotp") {
        count(2);
        const auto secret = Sensitive::owned(new std::string(text(a[1])));
        return session.setTotp(text(a[0]), *secret);
    }
    if (op == "removeTotp") { count(1); return session.removeTotp(text(a[0])); }
    if (op == "rename") { count(2); return session.rename(text(a[0]), text(a[1])); }
    if (op == "saveCopy") { count(1); session.saveCopy(text(a[0])); return encode(session.lastSaveResult()); }
    if (op == "saveAs") { count(1); session.saveAs(text(a[0])); return encode(session.lastSaveResult()); }
    if (op == "save") { count(0); session.save(); return encode(session.lastSaveResult()); }
    if (op == "hasSource") { count(0); return session.hasSource(); }
    if (op == "isModified") { count(0); return session.isModified(); }
    if (op == "recoveryDirectory") { count(0); return str(session.recoveryDirectory()); }
    if (op == "recoveries") {
        count(1); bool complete; QJsonArray records;
        for (const auto& record : SafeSave::recoveries(string(a[0]), complete)) records.append(encode(record));
        return QJsonObject{{"complete", complete}, {"records", records}};
    }
    if (op == "discardRecovery") { count(1); return SafeSave::discard(string(a[0])); }
    if (op == "copyUsername" || op == "copyPassword") {
        count(1); auto value = Sensitive::owned(new std::string(op == "copyUsername" ? session.copyUsername(text(a[0])) : session.copyPassword(text(a[0]))));
        return str(*value);
    }
    if (op == "autoTypeSequence") {
        count(1);
        return encode(session.autoTypeSequence(text(a[0])));
    }
    throw Error{};
}
}

int main(int argc, char** argv)
{
    ProcessLimits limits;
    // Apply before QCoreApplication, crypto initialization or any input.
    if (!WorkerPolicy::initialize(argc, argv, limits)) return 78;
    // Botan otherwise sizes its internal pool from host CPU availability,
    // reserving many thread stacks/allocator arenas even for a small DB.
    // This tunes execution concurrency, never the persisted Argon2 lanes.
    if (::setenv("BOTAN_THREAD_POOL_SIZE", "2", 1)) return 78;
    try {
        QCoreApplication app(argc, argv);
        QThreadPool::globalInstance()->setMaxThreadCount(2);
        if (!CoreSession::initialize()) return 70;
        CoreSession session;
        CheckpointStore checkpoints;
        BrowserSession browser(session, checkpoints);
        if (!send(STDOUT_FILENO, {{"v", Version}, {"id", 0}, {"ready", true},
                {"addressBytes", double(limits.addressBytes)}, {"cpuSeconds", int(limits.cpuSeconds)}})) return 0;
        QJsonObject request;
        quint64 previous = 0;
        while (true) {
            if (!WorkerPolicy::armCpu(limits.cpuSeconds)) return 78;
            if (!receive(STDIN_FILENO, request)) break;
            const auto id = request["id"].toDouble(-1);
            if (request["v"].toInt(-1) != Version || id != double(previous + 1) || !request["args"].isArray()) throw Error{};
            ++previous;
            const auto operation = string(request["op"]);
            const auto args = request["args"].toArray();
            const bool saving = operation == "save" || operation == "saveAs" || operation == "saveCopy"
                || operation == "enableRecovery" || operation == "browserDecision"
                || (checkpoints.enabled() && mutation(operation));
            if (!WorkerPolicy::armCpu(saving ? limits.saveCpuSeconds() : limits.cpuSeconds)) return 78;
            QJsonValue value;
            if (operation == "enableRecovery") {
                if (args.size() != 1 || !checkpoints.enable(string(args[0]), session)) _exit(76);
                value = true;
            } else if (operation == "browserRequest") {
                if (args.size() != 2 || args[0].toInt() <= 0 || !args[1].isObject()) throw Error{};
                value = browser.request(args[0].toInt(), args[1].toObject());
            } else if (operation == "browserDecision") {
                if (args.size() != 3 || args[0].toInt() <= 0) throw Error{};
                value = browser.decide(args[0].toInt(), string(args[1]), string(args[2]));
            } else if (operation == "browserClose") {
                if (args.size() != 1 || args[0].toInt() <= 0) throw Error{};
                browser.close(args[0].toInt());
                value = true;
            } else {
                value = dispatch(session, operation, args);
                const bool changed = (value.isBool() && value.toBool()) || (value.isString() && !value.toString().isEmpty());
                if (mutation(operation) && changed) {
                    if (checkpoints.enabled() && !checkpoints.commit(session)) _exit(76);
                    browser.modelChanged();
                }
            }
            const auto failure = operation == "unlock" ? session.lastReadFailure() : ReadFailure::None;
            if (!send(STDOUT_FILENO, {{"v", Version}, {"id", id}, {"failure", int(failure)}, {"value", value}, {"checkpoint", str(checkpoints.latest())}})) return 0;
            // No request/password or response remains queued while idle.
            request = {};
        }
    } catch (const std::bad_alloc&) { _exit(75); }
    catch (const SizeError&) { _exit(75); }
    catch (...) { _exit(70); }
    return 0;
}
