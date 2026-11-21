#include "gnome/RemoteSession.h"
#include "gnome/BrowserAccess.h"
#include "gnome/SessionProtocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <optional>

extern char** environ;
using namespace SessionProtocol;

struct RemoteSession::Impl
{
    QString executable;
    ProcessLimits limits;
    Cancellation cancellation;
    std::optional<std::uint64_t> taskEpoch;
    std::atomic<long> observedPid{0};
    std::shared_ptr<std::atomic<bool>> reaping = std::make_shared<std::atomic<bool>>(false);
    pid_t child = -1;
    int socket = -1;
    quint64 serial = 0;
    bool unlocked = false;
    bool protectedEdits = false;
    std::string checkpoint;
    ReadFailure failure = ReadFailure::None;
    ReadFailure fatal = ReadFailure::None;
    QJsonObject enforced;
    SaveResult saveResult;
    std::string recoverySource;
    std::unique_ptr<RemoteSession> recoveryWorker;
    unsigned activeWall = 30000;

    void stop()
    {
        unlocked = false;
        protectedEdits = false;
        recoverySource.clear();
        enforced = {};
        if (socket >= 0) { ::close(socket); socket = -1; }
        if (child > 0) {
            ::kill(child, SIGKILL);
            int status;
            QElapsedTimer timer; timer.start();
            pid_t result;
            do {
                result = ::waitpid(child, &status, WNOHANG);
                if (result == child || (result < 0 && errno == ECHILD)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (timer.elapsed() < 250);
            if (result != child && !(result < 0 && errno == ECHILD)) {
                // Uninterruptible kernel I/O cannot be synchronously bounded.
                // Keep at most this one child and reap off the UI/controller.
                reaping->store(true);
                std::thread([pid = child, state = reaping] {
                    int ignored;
                    while (::waitpid(pid, &ignored, 0) < 0 && errno == EINTR) {}
                    state->store(false);
                }).detach();
            }
        }
        child = -1;
        observedPid.store(0);
        serial = 0;
    }

    QJsonValue trip(ReadFailure reason)
    {
        failure = fatal = reason;
        stop();
        return {};
    }

    bool alive()
    {
        int status = 0;
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return true;
        child = -1;
        const bool resource = result > 0 && ((WIFSIGNALED(status) && (WTERMSIG(status) == SIGXCPU || WTERMSIG(status) == SIGXFSZ))
            || (WIFEXITED(status) && WEXITSTATUS(status) == 75));
        const bool recovery = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 76;
        trip(recovery ? ReadFailure::RecoveryFailure : resource ? ReadFailure::ResourceLimit : ReadFailure::WorkerFailure);
        return false;
    }

    bool validTime(QElapsedTimer& timer, std::uint64_t epoch)
    {
        if (cancellation->load() != epoch) { trip(ReadFailure::Cancelled); return false; }
        if (timer.elapsed() >= activeWall) { trip(ReadFailure::Timeout); return false; }
        return true;
    }

    bool transfer(char* buffer, std::size_t size, bool write, QElapsedTimer& timer, std::uint64_t epoch)
    {
        while (size) {
            if (!validTime(timer, epoch)) return false;
            const auto count = write ? quietWrite(socket, buffer, size) : ::read(socket, buffer, size);
            if (count > 0) { buffer += count; size -= count; continue; }
            if (!count || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                // The socket can close just before waitpid sees the final
                // status. Preserve CPU/ENOMEM diagnostics instead of racing
                // to classify every such exit as an unknown crash.
                for (unsigned attempt = 0; attempt < 25 && child > 0; ++attempt) {
                    if (!alive()) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                trip(ReadFailure::WorkerFailure); return false;
            }
            if (!alive()) return false;
            pollfd wait{socket, short(write ? POLLOUT : POLLIN), 0};
            if (::poll(&wait, 1, 10) < 0 && errno != EINTR) { trip(ReadFailure::WorkerFailure); return false; }
        }
        return validTime(timer, epoch);
    }

    QJsonObject receive(QElapsedTimer& timer, std::uint64_t epoch)
    {
        char prefix[4];
        if (!transfer(prefix, sizeof(prefix), false, timer, epoch)) return {};
        const auto length = qFromLittleEndian<quint32>(prefix);
        if (!length || length > MaximumFrame) { trip(ReadFailure::ResourceLimit); return {}; }
        QByteArray bytes(length, '\0');
        const Sensitive::Erase clearFrame{bytes.data(), std::size_t(bytes.size())};
        if (!transfer(bytes.data(), bytes.size(), false, timer, epoch)) return {};
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !json.isObject()) { trip(ReadFailure::WorkerFailure); return {}; }
        return json.object();
    }

    bool start(QElapsedTimer& timer, std::uint64_t epoch)
    {
        if (child > 0) return true;
        if (reaping->load() || !limits.valid() || !validTime(timer, epoch)) return false;
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair)) return false;
        posix_spawn_file_actions_t files;
        posix_spawnattr_t attributes;
        const bool haveFiles = !posix_spawn_file_actions_init(&files);
        const bool haveAttrs = !posix_spawnattr_init(&attributes);
        bool ok = haveFiles && haveAttrs;
        if (ok) {
            sigset_t empty, defaults;
            sigemptyset(&empty); sigemptyset(&defaults);
            for (int signal : {SIGPIPE, SIGXCPU, SIGXFSZ, SIGTERM, SIGINT}) sigaddset(&defaults, signal);
            ok = !posix_spawn_file_actions_adddup2(&files, pair[1], STDIN_FILENO)
                && !posix_spawn_file_actions_adddup2(&files, pair[1], STDOUT_FILENO)
                && !posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0)
                && !posix_spawn_file_actions_addclosefrom_np(&files, 3)
                && !posix_spawnattr_setsigmask(&attributes, &empty)
                && !posix_spawnattr_setsigdefault(&attributes, &defaults)
                && !posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        }
        auto path = QFile::encodeName(executable);
        auto memory = QByteArray::number(limits.addressBytes);
        auto cpu = QByteArray::number(limits.cpuSeconds);
        auto parent = QByteArray::number(::getpid());
        char* argv[] = {path.data(), memory.data(), cpu.data(), parent.data(), nullptr};
        if (ok) ok = !::posix_spawn(&child, path.constData(), &files, &attributes, argv, environ);
        if (haveFiles) posix_spawn_file_actions_destroy(&files);
        if (haveAttrs) posix_spawnattr_destroy(&attributes);
        ::close(pair[1]);
        if (!ok) { ::close(pair[0]); child = -1; return false; }
        socket = pair[0]; observedPid.store(child);
        const auto ready = receive(timer, epoch);
        if (fatal != ReadFailure::None) return false;
        if (ready["v"].toInt(-1) != Version || ready["id"].toInt(-1) != 0 || ready["ready"] != true
            || ready["addressBytes"].toDouble() != double(limits.addressBytes)
            || ready["cpuSeconds"].toInt() != int(limits.cpuSeconds)) {
            trip(ReadFailure::WorkerFailure); return false;
        }
        enforced = ready;
        return true;
    }

    QJsonValue call(const char* operation, QJsonArray arguments, bool needsModel = true)
    {
        if (fatal != ReadFailure::None || (needsModel && !unlocked)) return {};
        const auto epoch = taskEpoch.value_or(cancellation->load());
        const auto op = QByteArray(operation);
        activeWall = (op == "save" || op == "saveAs" || op == "saveCopy" || op == "enableRecovery"
            || op == "browserDecision"
            || (protectedEdits && mutation(QString::fromLatin1(operation)))) ? limits.saveWallMilliseconds() : limits.wallMilliseconds;
        QElapsedTimer timer; timer.start();
        try {
            if (!start(timer, epoch)) {
                if (fatal == ReadFailure::None) trip(ReadFailure::WorkerFailure);
                return {};
            }
            auto bytes = frame({{"v", Version}, {"id", double(++serial)}, {"op", operation}, {"args", arguments}});
            const Sensitive::Erase clearFrame{bytes.data(), std::size_t(bytes.size())};
            if (!transfer(bytes.data(), bytes.size(), true, timer, epoch)) return {};
            const auto response = receive(timer, epoch);
            if (fatal != ReadFailure::None) return {};
            if (!validTime(timer, epoch)) return {};
            const auto code = response["failure"].toInt(-1);
            if (response["v"].toInt(-1) != Version || response["id"].toDouble(-1) != double(serial)
                || code < 0 || code > int(ReadFailure::KeyFileTooLarge) || !response.contains("value"))
                return trip(ReadFailure::WorkerFailure);
            failure = ReadFailure(code);
            if (response.contains("checkpoint")) {
                if (!response["checkpoint"].isString()) return trip(ReadFailure::WorkerFailure);
                checkpoint = response["checkpoint"].toString().toStdString();
            }
            return response["value"];
        } catch (const std::bad_alloc&) { return trip(ReadFailure::ResourceLimit); }
        catch (const SizeError&) { return trip(ReadFailure::ResourceLimit); }
        catch (const Error&) { return trip(ReadFailure::WorkerFailure); }
    }

    bool boolean(const char* op, QJsonArray args, bool needsModel = true)
    {
        const auto value = call(op, args, needsModel);
        if (fatal != ReadFailure::None || (needsModel && !unlocked)) return false;
        if (!value.isBool()) { trip(ReadFailure::WorkerFailure); return false; }
        return value.toBool();
    }

    BrowserReply browserReply(const char* operation, QJsonArray arguments)
    {
        const auto value = call(operation, arguments);
        if (fatal != ReadFailure::None || !unlocked) return {};
        const auto object = value.toObject();
        const auto response = object.value("response");
        const auto approval = object.value("approval");
        const auto origin = object.value("origin");
        const auto choices = object.value("choices");
        const auto registered = object.value("registered");
        if (!value.isObject()
            || (!response.isUndefined() && !response.isObject())
            || (!approval.isUndefined() && (!approval.isString() || approval.toString().size() != 32))
            || (origin.isUndefined() != choices.isUndefined())
            || (!origin.isUndefined() && (!approval.isString() || !origin.isString()
                || origin.toString().isEmpty() || origin.toString().size() > 512 || !choices.isArray()))
            || (!registered.isUndefined() && (!registered.isBool() || !response.isObject()))
            || (response.isObject() && approval.isString())) {
            trip(ReadFailure::WorkerFailure);
            return {};
        }
        BrowserReply result{response.toObject(), approval.toString(), origin.toString(), {}, registered.toBool()};
        if (!choices.isArray()) return result;
        const auto rows = choices.toArray();
        if (rows.isEmpty() || rows.size() > BrowserLoginLimit) {
            trip(ReadFailure::WorkerFailure);
            return {};
        }
        for (const auto& row : rows) {
            const auto item = row.toObject();
            const auto id = item.value("id").toString();
            const auto title = item.value("title");
            const auto group = item.value("group");
            if (item.size() != 3 || id.size() != 36 || !title.isString() || !group.isString()
                || title.toString().size() > 121 || group.toString().size() > 121) {
                trip(ReadFailure::WorkerFailure);
                return {};
            }
            result.choices.push_back({id, title.toString(), group.toString()});
        }
        return result;
    }

    bool persist(const char* operation, QJsonArray args, const std::string& target = {})
    {
        saveResult = {};
        if (!unlocked) { saveResult.status = SaveStatus::NoSource; return false; }
        // Once a save request can reach the worker, a lost reply cannot prove
        // whether its rename happened. Recovery records resolve that ambiguity.
        saveResult.status = SaveStatus::Uncertain;
        saveResult.target = target;
        const auto value = call(operation, args);
        if (fatal != ReadFailure::None) return false;
        try {
            saveResult = saved(value);
            if (saveResult.committed() && QByteArray(operation) == "saveAs") recoverySource.clear();
            return saveResult.committed();
        }
        catch (const Error&) { trip(ReadFailure::WorkerFailure); return false; }
    }
    std::string text(const char* op, QJsonArray args)
    {
        const auto value = call(op, args);
        if (fatal != ReadFailure::None || !unlocked) return {};
        if (!value.isString()) { trip(ReadFailure::WorkerFailure); return {}; }
        return value.toString().toStdString();
    }
};

RemoteSession::RemoteSession(QString executable, ProcessLimits limits, Cancellation cancellation) : impl(std::make_unique<Impl>())
{
    impl->executable = executable.isEmpty() ? QCoreApplication::applicationDirPath() + "/keepassxc-gnome-worker" : executable;
    impl->limits = limits;
    impl->cancellation = cancellation ? std::move(cancellation) : std::make_shared<std::atomic<std::uint64_t>>(0);
}
RemoteSession::~RemoteSession() { impl->stop(); }
void RemoteSession::beginTask(std::uint64_t epoch) { impl->taskEpoch = epoch; impl->fatal = ReadFailure::None; }
void RemoteSession::cancel() { impl->cancellation->fetch_add(1); }
long RemoteSession::processId() const { return impl->observedPid.load(); }
ReadFailure RemoteSession::lastReadFailure() const { return impl->failure; }
ReadFailure RemoteSession::fatalFailure() const { return impl->fatal; }
QJsonObject RemoteSession::enforcedLimits() const { return impl->enforced; }
bool RemoteSession::unlock(const std::string& path, const std::string& password)
{
    auto credentials = Sensitive::owned(new UnlockCredentials{true, password, {}});
    return unlock(path, *credentials);
}
bool RemoteSession::unlock(const std::string& path, const UnlockCredentials& credentials)
{
    lock();
    impl->unlocked = impl->boolean("unlock", {str(path), credentials.usePassword, str(credentials.password), str(credentials.keyFile)}, false);
    if (impl->unlocked) impl->recoverySource = impl->text("recoveryDirectory", {});
    return impl->unlocked;
}
bool RemoteSession::create(const std::string& name, const std::string& password)
{
    lock();
    return impl->unlocked = impl->boolean("create", {str(name), str(password)}, false);
}
bool RemoteSession::enableRecovery(const std::string& folder)
{
    return impl->protectedEdits = impl->boolean("enableRecovery", {str(folder)});
}
std::string RemoteSession::lastCheckpoint() const { return impl->checkpoint; }
bool RemoteSession::fullyStopped() const {
    return impl->child < 0 && !impl->reaping->load() && (!impl->recoveryWorker || impl->recoveryWorker->fullyStopped());
}
void RemoteSession::lock() { impl->stop(); impl->fatal = impl->failure = ReadFailure::None; }

BrowserReply RemoteSession::browserRequest(int connection, const QJsonObject& message)
{
    if (connection <= 0) return {};
    return impl->browserReply("browserRequest", {connection, message});
}

BrowserReply RemoteSession::browserDecision(int connection, const QString& approval, const QString& selection)
{
    if (connection <= 0 || approval.size() != 32 || selection.size() > 64) return {};
    return impl->browserReply("browserDecision", {connection, approval, selection});
}

void RemoteSession::closeBrowserConnection(int connection)
{
    if (connection > 0) impl->boolean("browserClose", {connection});
}

std::vector<GroupSummary> RemoteSession::groups() const
{
    std::vector<GroupSummary> result;
    const auto value = impl->call("groups", {});
    if (impl->fatal != ReadFailure::None || !impl->unlocked) return result;
    try {
        if (!value.isArray() || value.toArray().size() > 20000) throw Error{};
        for (const auto& row : value.toArray()) {
            const auto a = row.toArray();
            if (a.size() != 5 || !a[2].isDouble() || a[2].toInt(-1) < 0 || a[2].toInt() > 128 || !a[3].isDouble() || a[3].toInt(-1) < 0 || !a[4].isBool()) throw Error{};
            result.push_back({SessionProtocol::text(a[0]), SessionProtocol::text(a[1]), unsigned(a[2].toInt()), a[3].toInt(), a[4].toBool()});
        }
    } catch (const Error&) { impl->trip(ReadFailure::WorkerFailure); return {}; }
    return result;
}
std::vector<EntrySummary> RemoteSession::search(const std::string& query, const std::string& group) const
{
    std::vector<EntrySummary> result;
    const auto value = impl->call("search", {str(query), str(group)});
    if (impl->fatal != ReadFailure::None || !impl->unlocked) return result;
    try {
        if (!value.isArray() || value.toArray().size() > 100000) throw Error{};
        for (const auto& row : value.toArray()) {
            const auto a = row.toArray();
            if (a.size() != 6 || !a[4].isDouble() || a[4].toInt(-1) < 0 || !a[5].isBool()) throw Error{};
            result.push_back({SessionProtocol::text(a[0]), SessionProtocol::text(a[1]), SessionProtocol::text(a[2]), SessionProtocol::text(a[3]), a[4].toInt(), a[5].toBool()});
        }
    } catch (const Error&) { impl->trip(ReadFailure::WorkerFailure); return {}; }
    return result;
}
bool RemoteSession::entry(const std::string& id, EntryDetails& result) const
{
    result = {};
    const auto value = impl->call("entry", {str(id)});
    if (value.isNull() || value.isUndefined() || impl->fatal != ReadFailure::None) return false;
    try { result = details(value); return true; }
    catch (const Error&) { impl->trip(ReadFailure::WorkerFailure); return false; }
}
std::string RemoteSession::addGroup(const std::string& parent, const std::string& name) { return impl->text("addGroup", {str(parent), str(name)}); }
bool RemoteSession::renameGroup(const std::string& id, const std::string& name) { return impl->boolean("renameGroup", {str(id), str(name)}); }
bool RemoteSession::updateGroup(const std::string& id, const std::string& name, int icon) { return impl->boolean("updateGroup", {str(id), str(name), icon}); }
bool RemoteSession::setGroupIcon(const std::string& id, int icon) { return impl->boolean("setGroupIcon", {str(id), icon}); }
bool RemoteSession::removeGroup(const std::string& id) { return impl->boolean("removeGroup", {str(id)}); }
std::string RemoteSession::addEntry(const std::string& group, const EntryDetails& value) { return impl->text("addEntry", {str(group), encode(value)}); }
bool RemoteSession::updateEntry(const EntryDetails& value) { return impl->boolean("updateEntry", {encode(value)}); }
bool RemoteSession::removeEntry(const std::string& id) { return impl->boolean("removeEntry", {str(id)}); }
bool RemoteSession::rename(const std::string& id, const std::string& title) { return impl->boolean("rename", {str(id), str(title)}); }
bool RemoteSession::saveCopy(const std::string& path) { return impl->persist("saveCopy", {str(path)}, path); }
bool RemoteSession::saveAs(const std::string& path) { return impl->persist("saveAs", {str(path)}, path); }
bool RemoteSession::save() { return impl->persist("save", {}); }
bool RemoteSession::hasSource() const { return impl->boolean("hasSource", {}); }
bool RemoteSession::isModified() const { return impl->boolean("isModified", {}); }
std::string RemoteSession::recoveryDirectory() const { return impl->recoverySource; }
SaveResult RemoteSession::lastSaveResult() const { return impl->saveResult; }
std::vector<RecoveryRecord> RemoteSession::recoveries(const std::string& folder, bool& complete)
{
    complete = false;
    std::vector<RecoveryRecord> result;
    if (!impl->recoveryWorker) impl->recoveryWorker = std::make_unique<RemoteSession>(impl->executable, impl->limits, impl->cancellation);
    auto& auxiliary = *impl->recoveryWorker;
    auxiliary.beginTask(impl->taskEpoch.value_or(impl->cancellation->load()));
    const auto value = auxiliary.impl->call("recoveries", {str(folder)}, false);
    const bool failed = auxiliary.impl->fatal != ReadFailure::None;
    auxiliary.lock();
    if (failed) return result;
    try {
        const auto v = value.toObject();
        if (!value.isObject() || !v["complete"].isBool() || !v["records"].isArray()
            || v["records"].toArray().size() > 128) throw Error{};
        for (const auto& record : v["records"].toArray()) result.push_back(recovery(record));
        complete = v["complete"].toBool();
    } catch (const Error&) { return {}; }
    return result;
}
bool RemoteSession::discardRecovery(const std::string& directory)
{
    if (!impl->recoverySource.empty() && QFileInfo(QString::fromStdString(directory)).canonicalFilePath()
        == QString::fromStdString(impl->recoverySource)) return false;
    if (!impl->recoveryWorker) impl->recoveryWorker = std::make_unique<RemoteSession>(impl->executable, impl->limits, impl->cancellation);
    auto& auxiliary = *impl->recoveryWorker;
    auxiliary.beginTask(impl->taskEpoch.value_or(impl->cancellation->load()));
    const bool ok = auxiliary.impl->boolean("discardRecovery", {str(directory)}, false);
    auxiliary.lock();
    return ok;
}
std::string RemoteSession::copyUsername(const std::string& id) const { return impl->text("copyUsername", {str(id)}); }
std::string RemoteSession::copyPassword(const std::string& id) const { return impl->text("copyPassword", {str(id)}); }

AutoTypeSequence RemoteSession::autoTypeSequence(const std::string& id) const
{
    if (!impl->unlocked || id.empty() || id.size() > 64) return {};
    const auto value = impl->call("autoTypeSequence", {str(id)});
    if (impl->fatal != ReadFailure::None || !impl->unlocked) return {};
    try {
        return SessionProtocol::autoTypeSequence(value);
    } catch (const Error&) {
        impl->trip(ReadFailure::WorkerFailure);
        return {};
    }
}
