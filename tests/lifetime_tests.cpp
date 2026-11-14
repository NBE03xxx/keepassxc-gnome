#include <gtk/gtk.h>
#include <gio/gunixoutputstream.h>
#include <fcntl.h>
#include <algorithm>
#include "gnome/SessionGuard.h"
#include "gnome/SecretClipboard.h"
#include "gnome/Sensitive.h"
#include "gnome/RemoteSession.h"
#include "gnome/SafeSave.h"
#include "ReaderFixtures.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <functional>
#include <thread>
#include <csignal>

using namespace ReaderTest;
namespace {
unsigned checks = 0;
void check(bool ok, const char* label) { require(ok, label); ++checks; }
void pump(unsigned milliseconds = 30) {
    const auto until = g_get_monotonic_time() + milliseconds * 1000;
    do { while (g_main_context_iteration(nullptr, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < until);
}
void until(std::function<bool()> condition, const char* label) {
    const auto end = g_get_monotonic_time() + 4000000;
    while (!condition() && g_get_monotonic_time() < end) pump(5);
    check(condition(), label);
}
QByteArray load(const QString& path) { QFile f(path); require(f.open(QIODevice::ReadOnly), "read fixture"); return f.readAll(); }
void put(const QString& path, const QByteArray& data) { QFile f(path); require(f.open(QIODevice::WriteOnly) && f.write(data) == data.size(), "write fixture"); }
void clocks() {
    IdleLock idle;
    idle.activity(1); check(!idle.poll(1, 1), "fresh activity does not lock");
    check(!idle.poll(IdleLock::Timeout, IdleLock::Timeout), "idle deadline is not early");
    check(idle.poll(IdleLock::Timeout + 1, IdleLock::Timeout + 1), "five minute inactivity locks");
    idle.activity(IdleLock::Timeout + 10);
    check(!idle.poll(IdleLock::Timeout + 10, IdleLock::Timeout + 10), "new input resets inactivity");
    check(idle.poll(IdleLock::Timeout + 2000010, IdleLock::Timeout + 11), "boottime detects missed suspend notification");
    std::string secret = "synthetic-memory-canary-long-enough-for-heap";
    const auto size = secret.size(); char* memory = secret.data(); Sensitive::wipe(secret);
    check(secret.empty() && std::all_of(memory, memory + size, [](char c) { return !c; }), "owned string overwritten before release");
    EntryDetails details{"id", "group", "title", "username", "synthetic-password", "url", "notes"}; Sensitive::wipe(details);
    check(details.password.empty() && details.notes.empty() && details.username.empty(), "all entry fields erased");
}
int unavailableBus() {
    unsigned locks = 0; bool available = true;
    SessionGuard guard([&] { ++locks; }, [&](bool value) { available = value; });
    check(guard.blocked(), "bus initialization gates authentication");
    until([&] { return guard.settled(); }, "unreachable bus initialization completes");
    check(!available && guard.blocked() && locks, "bus connection failure cannot certify an unlocked desktop");
    guard.refresh(); pump(20);
    check(guard.blocked(), "failed connection stays blocked on refresh");
    return 0;
}
void connectionFailures(const QString& executable, const char* address) {
    QTemporaryDir folder; require(folder.isValid(), "unreachable bus test directory");
    const auto unreachable = "unix:path=" + folder.filePath("missing.sock");
    // Fresh executables avoid GLib's cached bus connections. Both the valid
    // address and the nonexistent socket belong to this synthetic test.
    for (unsigned buses : {1U, 2U, 3U}) {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("DBUS_SESSION_BUS_ADDRESS", buses & 1 ? unreachable : QString::fromUtf8(address));
        environment.insert("DBUS_SYSTEM_BUS_ADDRESS", buses & 2 ? unreachable : QString::fromUtf8(address));
        QProcess child; child.setProcessEnvironment(environment);
        child.start(executable, {"--bus-connection-failure"});
        const bool completed = child.waitForFinished(6000);
        if (!completed || child.exitStatus() != QProcess::NormalExit || child.exitCode())
            std::fprintf(stderr, "Unreachable bus scenario %u: %s%s\n", buses, child.readAllStandardOutput().constData(), child.readAllStandardError().constData());
        check(completed && child.exitStatus() == QProcess::NormalExit && !child.exitCode(), "session/system/both connection failures block authentication");
    }
}
struct Read { bool done = false, ok = false; GError* error = nullptr; ~Read() { g_clear_error(&error); } };
void provide(GdkContentProvider* provider, GOutputStream* stream, Read& read, GCancellable* cancellation = nullptr) {
    gdk_content_provider_write_mime_type_async(provider, "text/plain;charset=utf-8", stream, G_PRIORITY_DEFAULT, cancellation,
        +[](GObject* provider, GAsyncResult* result, gpointer data) {
            auto& read = *static_cast<Read*>(data); read.ok = gdk_content_provider_write_mime_type_finish(GDK_CONTENT_PROVIDER(provider), result, &read.error); read.done = true;
        }, &read);
}
void clipboard() {
    auto* provider = secret_clipboard_new("synthetic-clipboard-canary", 1000000);
    auto* formats = gdk_content_provider_ref_storable_formats(provider);
    check(!gdk_content_formats_contain_mime_type(formats, "text/plain"), "provider does not request persistence"); gdk_content_formats_unref(formats);
    auto* output = g_memory_output_stream_new_resizable(); Read read;
    provide(provider, output, read); until([&] { return read.done; }, "live clipboard request completes");
    check(read.ok && QByteArray(static_cast<char*>(g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(output))), g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(output))) == "synthetic-clipboard-canary", "live clipboard exports correct text");
    secret_clipboard_revoke(provider); check(!secret_clipboard_live(provider), "explicit revocation stops provider");
    auto* expiredOutput = g_memory_output_stream_new_resizable(); Read expired;
    provide(provider, expiredOutput, expired); until([&] { return expired.done; }, "revoked clipboard request completes");
    check(!expired.ok && expired.error && g_error_matches(expired.error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
        && !g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(expiredOutput)), "late request returns no secret bytes");
    g_object_unref(provider); g_object_unref(output); g_object_unref(expiredOutput);
    provider = secret_clipboard_new("synthetic-expiry-canary", 1000); pump(5);
    check(!secret_clipboard_live(provider), "deadline enforced without UI timer"); g_object_unref(provider);
    provider = secret_clipboard_new("synthetic-cancellation-canary"); auto* cancellation = g_cancellable_new(); g_cancellable_cancel(cancellation);
    output = g_memory_output_stream_new_resizable(); Read cancelled; provide(provider, output, cancelled, cancellation);
    until([&] { return cancelled.done; }, "cancelled transfer completes");
    check(!cancelled.ok, "receiver cancellation respected"); g_object_unref(cancellation); g_object_unref(output); g_object_unref(provider);
    provider = secret_clipboard_new(std::string(1 << 20, 's'));
    int pipe[2]; require(!::pipe2(pipe, O_CLOEXEC | O_NONBLOCK), "backpressured clipboard pipe");
    auto* blocked = g_unix_output_stream_new(pipe[1], TRUE); Read pending;
    provide(provider, G_OUTPUT_STREAM(blocked), pending); pump(5); secret_clipboard_revoke(provider);
    until([&] { return pending.done; }, "revocation cancels blocked transfer");
    check(!pending.ok && !secret_clipboard_pending(provider), "backpressured transfer cannot report success and releases secret work buffer after revocation");
    g_object_unref(blocked); ::close(pipe[0]); g_object_unref(provider);
}

struct Service {
    GDBusConnection* connection;
    GDBusNodeInfo* info;
    bool locked = false, sleeping = false, denyInhibit = false, holdQueries = false;
    std::string fdoError;
    unsigned fdoQueries = 0;
    std::vector<GDBusMethodInvocation*> queries;
    int inhibitorReader = -1;
    unsigned inhibitorCalls = 0;
    std::vector<guint> objects;
    explicit Service(const char* address) {
        GError* error = nullptr;
        connection = g_dbus_connection_new_for_address_sync(address, GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, &error);
        require(connection && !error, "private notification service connection");
        info = g_dbus_node_info_new_for_xml("<node><interface name='org.gnome.ScreenSaver'><method name='GetActive'><arg type='b' direction='out'/></method><signal name='ActiveChanged'><arg type='b'/></signal></interface><interface name='org.freedesktop.ScreenSaver'><method name='GetActive'><arg type='b' direction='out'/></method><signal name='ActiveChanged'><arg type='b'/></signal></interface><interface name='org.freedesktop.login1.Manager'><method name='Inhibit'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='h' direction='out'/></method><property name='PreparingForSleep' type='b' access='read'/><signal name='PrepareForSleep'><arg type='b'/></signal></interface></node>", &error);
        require(info && !error, "private notification schema");
        static const GDBusInterfaceVTable vtable{
            +[](GDBusConnection*, const char*, const char* path, const char*, const char*, GVariant*, GDBusMethodInvocation* invocation, gpointer data) {
                auto& service = *static_cast<Service*>(data);
                if (!std::strcmp(path, "/org/freedesktop/ScreenSaver")) {
                    ++service.fdoQueries;
                    if (!service.fdoError.empty()) { g_dbus_method_invocation_return_dbus_error(invocation, service.fdoError.c_str(), "Synthetic FDO capability error"); return; }
                }
                if (!std::strcmp(g_dbus_method_invocation_get_method_name(invocation), "Inhibit")) {
                    ++service.inhibitorCalls;
                    if (service.denyInhibit) { g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.AccessDenied", "Synthetic denial"); return; }
                    int pipe[2]; require(!::pipe2(pipe, O_CLOEXEC | O_NONBLOCK), "private delay inhibitor pipe");
                    if (service.inhibitorReader >= 0) ::close(service.inhibitorReader);
                    service.inhibitorReader = pipe[0]; auto* list = g_unix_fd_list_new();
                    const int handle = g_unix_fd_list_append(list, pipe[1], nullptr); ::close(pipe[1]); require(handle >= 0, "pass delay inhibitor fd");
                    g_dbus_method_invocation_return_value_with_unix_fd_list(invocation, g_variant_new("(h)", handle), list); g_object_unref(list);
                } else if (service.holdQueries) service.queries.push_back(G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)));
                else g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", service.locked));
            }, +[](GDBusConnection*, const char*, const char*, const char*, const char*, GError**, gpointer data) -> GVariant* {
                return g_variant_new_boolean(static_cast<Service*>(data)->sleeping);
            }, nullptr, {}};
        const char* paths[]{"/org/gnome/ScreenSaver", "/org/freedesktop/ScreenSaver", "/org/freedesktop/login1"};
        for (unsigned i = 0; i < 3; ++i) { auto id = g_dbus_connection_register_object(connection, paths[i], info->interfaces[i], &vtable, this, nullptr, &error); require(id && !error, "notification object"); objects.push_back(id); }
    }
    void name(const char* name, bool own) {
        GError* error = nullptr; auto* reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", own ? "RequestName" : "ReleaseName", own ? g_variant_new("(su)", name, 0U) : g_variant_new("(s)", name), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
        require(reply && !error, "own/release private desktop name"); g_variant_unref(reply);
    }
    void answerQueries(bool value) {
        for (auto* query : queries) { g_dbus_method_invocation_return_value(query, g_variant_new("(b)", value)); g_object_unref(query); }
        queries.clear();
    }
    void signal(bool active, bool sleep = false) {
        if (sleep) sleeping = active; else locked = active;
        GError* error = nullptr;
        require(g_dbus_connection_emit_signal(connection, nullptr, sleep ? "/org/freedesktop/login1" : "/org/gnome/ScreenSaver", sleep ? "org.freedesktop.login1.Manager" : "org.gnome.ScreenSaver", sleep ? "PrepareForSleep" : "ActiveChanged", g_variant_new("(b)", active), &error) && !error, "emit private desktop signal");
        require(g_dbus_connection_flush_sync(connection, nullptr, &error) && !error, "flush private desktop signal");
    }
    ~Service() { answerQueries(false); if (inhibitorReader >= 0) ::close(inhibitorReader); for (auto id : objects) g_dbus_connection_unregister_object(connection, id); g_dbus_node_info_unref(info); g_dbus_connection_close_sync(connection, nullptr, nullptr); g_object_unref(connection); }
};
void notifications(const char* address) {
    Service owner(address), spoof(address);
    for (auto* name : {"org.gnome.ScreenSaver", "org.freedesktop.ScreenSaver", "org.freedesktop.login1"}) owner.name(name, true);
    unsigned locks = 0; bool available = false;
    {
        SessionGuard guard([&] { ++locks; }, [&](bool value) { available = value; });
        until([&] { return available; }, "desktop notification watchers ready"); pump(20);
        check(!guard.blocked() && !locks, "initial unlocked desktop accepted");
        spoof.signal(true); pump(30); check(!locks && !guard.blocked(), "untrusted sender cannot forge screen lock");
        spoof.signal(true, true); pump(30); check(!locks && !guard.blocked(), "untrusted sender cannot forge sleep");
        owner.signal(true); until([&] { return locks == 1; }, "trusted screen lock delivered"); check(guard.blocked(), "locked desktop blocks unlock");
        owner.signal(false); until([&] { return !guard.blocked(); }, "unlock notification clears desktop gate"); check(locks == 1, "desktop unlock does not unlock database");
        char ignored;
        check(owner.inhibitorReader >= 0 && ::read(owner.inhibitorReader, &ignored, 1) < 0 && errno == EAGAIN, "sleep delay held before sleep signal");
        owner.signal(true, true); until([&] { return locks == 2; }, "pre-sleep event delivered"); check(guard.blocked(), "pre-sleep blocks unlock");
        check(::read(owner.inhibitorReader, &ignored, 1) < 0 && errno == EAGAIN, "delay retained until lock completion");
        guard.releaseSleepDelay(); until([&] { return !::read(owner.inhibitorReader, &ignored, 1); }, "delay released after completed lock");
        owner.signal(false, true); until([&] { return locks == 3; }, "resume remains fail-closed"); until([&] { return available; }, "sleep delay reacquired on resume");
        check(!guard.blocked(), "resume permits explicit reauthentication");
        owner.name("org.gnome.ScreenSaver", false); until([&] { return locks == 4; }, "desktop service loss locks database");
    }
    owner.signal(true); pump(30); check(locks == 4, "destroyed watcher never calls former window");
    owner.name("org.gnome.ScreenSaver", true); owner.locked = true;
    {
        SessionGuard guard([&] { ++locks; }, [](bool) {});
        until([&] { return guard.settled() && guard.blocked(); }, "initial GetActive detects already locked desktop");
    }
    pump(30);
    owner.locked = false; owner.holdQueries = true;
    {
        SessionGuard guard([&] { ++locks; }, [](bool) {});
        until([&] { return owner.queries.size() == 2; }, "initial desktop queries held for race test");
        const auto before = locks; owner.signal(true);
        until([&] { return locks > before; }, "new lock precedes old query response");
        owner.answerQueries(false); until([&] { return guard.settled(); }, "stale query responses completed");
        check(guard.blocked(), "stale unlocked response cannot reverse newer lock");
    }
    owner.holdQueries = false; owner.locked = false; owner.sleeping = true;
    if (owner.inhibitorReader >= 0) { ::close(owner.inhibitorReader); owner.inhibitorReader = -1; }
    {
        SessionGuard guard([&] { ++locks; }, [](bool) {});
        until([&] { return guard.settled() && guard.blocked(); }, "initial preparing-for-sleep query gates unlock"); pump(20);
        check(owner.inhibitorReader < 0, "no new inhibitor acquired while already preparing to sleep");
    }
    owner.sleeping = false;
    for (auto* error : {"org.freedesktop.DBus.Error.NotSupported", "org.freedesktop.DBus.Error.UnknownMethod", "org.freedesktop.DBus.Error.UnknownInterface"}) {
        owner.fdoError = error; available = false;
        SessionGuard guard([&] { ++locks; }, [&](bool value) { available = value; });
        until([&] { return guard.settled() && available; }, "native GNOME plus logind accepts unsupported FDO state extension");
        check(!guard.blocked(), "GNOME 50 idle-inhibit-only proxy cannot disable database operations");
        const auto queries = owner.fdoQueries; guard.refresh(); pump(30);
        check(owner.fdoQueries == queries && !guard.blocked(), "permanent unsupported extension is not retried every idle tick");
        auto before = locks; owner.signal(true); until([&] { return locks > before; }, "native lock still delivered with unsupported FDO");
        check(guard.blocked(), "native locked state is never ignored");
        owner.signal(false); until([&] { return !guard.blocked(); }, "native unlock permits explicit authentication");
        owner.name("org.gnome.ScreenSaver", false); until([&] { return guard.blocked(); }, "unsupported FDO alone cannot certify unlocked desktop");
        owner.name("org.gnome.ScreenSaver", true); until([&] { return !guard.blocked(); }, "verified native service can recover protection");
    }
    owner.fdoError = "org.freedesktop.DBus.Error.AccessDenied";
    {
        SessionGuard guard([&] { ++locks; }, [](bool) {});
        until([&] { return guard.settled(); }, "FDO denial answered");
        check(guard.blocked(), "access denial is not mistaken for an unsupported extension");
    }
    owner.fdoError.clear();
    owner.sleeping = false; owner.denyInhibit = true;
    {
        const auto before = owner.inhibitorCalls;
        bool available = false; SessionGuard guard([&] { ++locks; }, [&](bool value) { available = value; });
        until([&] { return owner.inhibitorCalls > before; }, "private sleep protection denied"); pump(15);
        check(!available && guard.blocked(), "denied sleep protection is visible and cannot permit unlock");
    }
    pump(30);
}
void checkpoints(const QString& worker, const QString& cli) {
    QTemporaryDir d; require(d.isValid(), "checkpoint test directory");
    auto fixture = makeFixture(KeePass2::FILE_VERSION_4_1, KeePass2::CIPHER_AES256, true);
    const auto bytes = encrypt(fixture, compress(fixture, fixture.plaintext)); const auto source = d.filePath("source.kdbx"); put(source, bytes);
    const auto folder = d.filePath("recovery"); require(QDir().mkdir(folder) && !::chmod(QFile::encodeName(folder), 0700), "private recovery folder");
    RemoteSession other(worker); check(other.unlock(source.toStdString(), Master) && other.enableRecovery(folder.toStdString()), "second session checkpoint");
    const auto otherCopy = other.lastCheckpoint();
    RemoteSession session(worker); check(session.unlock(source.toStdString(), Master) && session.enableRecovery(folder.toStdString()), "enable encrypted edit protection");
    const auto initial = session.lastCheckpoint(); check(!initial.empty() && QFile::exists(QString::fromStdString(initial)) && !session.isModified(), "initial checkpoint does not dirty clean model");
    for (unsigned i = 0; i < 8; ++i) check(session.rename(fixture.entry, "Confirmed synthetic edit " + std::to_string(i)), "edit acknowledgment follows durable checkpoint");
    const auto latest = session.lastCheckpoint();
    bool complete; auto records = SafeSave::recoveries(folder, complete);
    const auto archiveDirs = QDir(folder).entryList({".keepassxc-gnome-save-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    check(complete && records.empty() && archiveDirs.size() == 3 && QFile::exists(QString::fromStdString(otherCopy)) && !QFile::exists(QString::fromStdString(initial)), "keep latest two own copies, retain and hide active sessions");
    check(!SafeSave::discard(QFileInfo(QString::fromStdString(latest)).absolutePath()), "another instance cannot delete active checkpoint");
    check(session.isModified() && load(source) == bytes, "automatic recovery never saves original or clears dirty");

    CoreSession recovered; check(recovered.unlock(latest, Master) && !recovered.hasSource(), "checkpoint reopens as recovery model");
    EntryDetails details; check(recovered.entry(fixture.entry, details) && details.title == "Confirmed synthetic edit 7", "last confirmed edit survived");
    QProcess reference; reference.start(cli, {"db-info", "-q", QString::fromStdString(latest)}); require(reference.waitForStarted(), "reference CLI starts"); reference.write(QByteArray(Master) + '\n'); reference.closeWriteChannel();
    check(reference.waitForFinished(10000) && reference.exitCode() == 0, "unmodified upstream CLI reads automatic checkpoint");
    require(!::chmod(QFile::encodeName(folder), 0755), "inject unsafe checkpoint permissions");
    check(!session.rename(fixture.entry, "Must not be acknowledged") && session.fatalFailure() == ReadFailure::RecoveryFailure && !session.processId(), "checkpoint failure locks and never acknowledges edit");
    require(!::chmod(QFile::encodeName(folder), 0700), "restore test permissions");
    recovered.lock(); check(recovered.unlock(latest, Master) && recovered.entry(fixture.entry, details) && details.title == "Confirmed synthetic edit 7", "failed new edit preserves earlier acknowledged data");
    check(load(source) == bytes && load(QString::fromStdString(latest)).size() > 0, "failure does not touch original or complete recovery copy");
    const auto pid = other.processId(); require(pid > 0 && !::kill(pid, SIGKILL), "kill idle worker");
    check(other.entries().empty() && other.fatalFailure() != ReadFailure::None && QFile::exists(QString::fromStdString(otherCopy)), "worker death leaves accepted checkpoint");
    records = SafeSave::recoveries(folder, complete);
    check(complete && records.size() == 3, "locking/death releases recovery records for inspection");
    for (auto& r : records) check(r.state == "checkpoint" && r.files.size() == 1 && r.files.front().kind == "written.kdbx", "archive has no published target or extra payload");
    session.lock(); check(!session.create("Synthetic recovery failure", ""), "locked failed session has no hidden model");
    const auto cancelFolder = d.filePath("cancel-recovery");
    require(QDir().mkdir(cancelFolder) && !::chmod(QFile::encodeName(cancelFolder), 0700), "cancellation recovery folder");
    RemoteSession cancellable(worker);
    check(cancellable.create("Synthetic protected cancellation", Master) && cancellable.enableRecovery(cancelFolder.toStdString()), "default KDF edit protection");
    const auto lastConfirmed = cancellable.lastCheckpoint();
    const auto root = cancellable.groups().front().id;
    bool accepted = false;
    std::thread request([&] { accepted = !cancellable.addEntry(root, EntryDetails{{}, root, "Unconfirmed synthetic entry", "user", "secret", {}, {}}).empty(); });
    QElapsedTimer waiting; waiting.start(); bool inProgress = false;
    while (waiting.elapsed() < 5000 && !inProgress) {
        for (const auto& directory : QDir(cancelFolder).entryList({".keepassxc-gnome-save-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot))
            if (QFile::exists(cancelFolder + '/' + directory + "/candidate.kdbx")) inProgress = true;
        if (!inProgress) QThread::msleep(2);
    }
    cancellable.cancel(); request.join();
    check(inProgress && !accepted && cancellable.fatalFailure() == ReadFailure::Cancelled && cancellable.fullyStopped(), "cancel during encrypted checkpoint stops worker without acknowledgement");
    recovered.lock();
    check(recovered.unlock(lastConfirmed, Master) && recovered.entries().empty(), "last acknowledged state survives cancelled edit");

}
}
int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--bus-connection-failure")) return unavailableBus();
    require(argc == 3, "lifetime test arguments");
    // GTestDBus forks its watcher: start it before GLib/Qt async threads.
    auto* bus = g_test_dbus_new(G_TEST_DBUS_NONE); g_test_dbus_up(bus); const auto address = std::string(g_test_dbus_get_bus_address(bus));
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS", address.c_str(), TRUE);
    QCoreApplication app(argc, argv); require(CoreSession::initialize(), "lifetime crypto initialization");
    clocks(); connectionFailures(QCoreApplication::applicationFilePath(), address.c_str()); clipboard();
    notifications(address.c_str());
    checkpoints(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
    g_test_dbus_down(bus); g_object_unref(bus);
    std::printf("PASS automatic lock/secret lifetime: %u checks, private desktop bus, upstream encrypted checkpoints and revocable clipboard\n", checks);
}
