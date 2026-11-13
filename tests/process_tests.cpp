#include "ReaderFixtures.h"
#include "gnome/RemoteSession.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <atomic>
#include <thread>

using namespace ReaderTest;

namespace {
void empty(RemoteSession& session, const QTemporaryDir& directory)
{
    EntryDetails details;
    const auto path = directory.filePath("must-not-exist.kdbx");
    require(session.groups().empty() && session.entries().empty() && !session.entry("x", details)
        && session.copyPassword("x").empty() && session.copyUsername("x").empty()
        && !session.saveCopy(path.toStdString()) && !QFile::exists(path), "failed worker publishes no model or saved file");
}

bool reaped(long pid)
{
    errno = 0;
    return ::kill(pid, 0) < 0 && errno == ESRCH;
}

void fault(const QString& helper, const char* mode, ReadFailure expected, ProcessLimits limits)
{
    QTemporaryDir directory;
    RemoteSession session(helper, limits);
    QElapsedTimer timer; timer.start();
    require(!session.create(mode, "synthetic-private-password"), "faulty request does not succeed");
    if (session.lastReadFailure() != expected)
        std::fprintf(stderr, "Fault %s: expected=%d got=%d\n", mode, int(expected), int(session.lastReadFailure()));
    require(session.lastReadFailure() == expected, "fault classification");
    require(timer.elapsed() < 6500 && session.processId() == 0, "fault handled in bounded time");
    empty(session, directory);
    require(session.create("normal", "synthetic-private-password"), "fresh worker works after fault");
    const auto pid = session.processId();
    require(pid > 0 && session.enforcedLimits()["addressBytes"].toDouble() == double(limits.addressBytes), "real memory limit handshake");
    rlimit address{}, core{}, cpu{}, fileSize{};
    require(!prlimit(pid, RLIMIT_AS, nullptr, &address) && address.rlim_cur == limits.addressBytes
        && address.rlim_max == limits.addressBytes, "kernel enforces address-space soft and hard limits");
    require(!prlimit(pid, RLIMIT_FSIZE, nullptr, &fileSize) && fileSize.rlim_cur == (128ULL << 20)
        && fileSize.rlim_max == (128ULL << 20), "worker output file size bounded in kernel");
    require(!prlimit(pid, RLIMIT_CORE, nullptr, &core) && !core.rlim_cur && !core.rlim_max, "worker core dumps disabled");
    require(!prlimit(pid, RLIMIT_CPU, nullptr, &cpu) && cpu.rlim_cur <= limits.cpuSeconds + 1, "kernel CPU limit is armed per operation");
    // Passwords travel only over the private socket, never argv or environment.
    QFile args(QStringLiteral("/proc/%1/cmdline").arg(pid));
    require(args.open(QIODevice::ReadOnly) && !args.readAll().contains("synthetic-private-password"), "no password in worker command line");
    session.lock();
    require(reaped(pid), "worker reaped after lock");
    std::printf("PASS process fault: %s\n", mode);
}

void cancel(const QString& helper, const std::string& path = {})
{
    RemoteSession session(helper);
    QTemporaryDir directory;
    std::atomic<bool> finished{false};
    bool accepted = false;
    std::thread request([&] {
        accepted = path.empty() ? session.create("wall", "synthetic-cancel-password") : session.unlock(path, Master);
        finished.store(true);
    });
    QElapsedTimer wait; wait.start();
    while (!session.processId() && !finished.load() && wait.elapsed() < 3000) QThread::msleep(2);
    const auto pid = session.processId();
    require(pid > 0, "worker starts for cancellation");
    // For a real input, wait for CPU consumed by its KDF rather than merely
    // canceling startup. The synthetic header is intentionally key-mismatched.
    if (!path.empty()) {
        const auto statsPath = QStringLiteral("/proc/%1/stat").arg(pid);
        bool consumed = false;
        while (!finished.load() && wait.elapsed() < 3000) {
            QFile stats(statsPath);
            if (stats.open(QIODevice::ReadOnly)) {
                const auto line = stats.readAll();
                const auto columns = line.mid(line.lastIndexOf(')') + 2).split(' ');
                if (columns.size() > 12 && columns[11].toULongLong() + columns[12].toULongLong() >= 8) {
                    consumed = true; break;
                }
            }
            QThread::msleep(5);
        }
        require(consumed && !finished.load(), "cancellation reaches active real KDF");
    } else QThread::msleep(50);
    QElapsedTimer timer; timer.start();
    session.cancel();
    request.join();
    require(timer.elapsed() < 1500 && !accepted && session.lastReadFailure() == ReadFailure::Cancelled, "active work cancels promptly");
    require(reaped(pid), "cancelled worker reaped");
    empty(session, directory);
    std::printf("PASS cancellation: %s\n", path.empty() ? "blocked worker" : "real KeePassXC KDF");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 4 && std::string(argv[1]) == "--parent-driver") {
        RemoteSession session(QString::fromLocal8Bit(argv[2]));
        require(session.create("normal", "synthetic-parent-death"), "orphan test setup");
        QFile pid(QString::fromLocal8Bit(argv[3]));
        require(pid.open(QIODevice::WriteOnly) && pid.write(QByteArray::number(session.processId())) > 0, "write worker identity");
        pid.close();
        for (;;) ::pause();
    }
    require(argc == 3 && CoreSession::initialize(), "process test environment");
    ProcessLimits quick;
    quick.wallMilliseconds = 300;
    fault(argv[2], "wall", ReadFailure::Timeout, quick);
    quick.wallMilliseconds = 5000; quick.cpuSeconds = 1;
    fault(argv[2], "cpu", ReadFailure::ResourceLimit, quick);
    quick.addressBytes = 128ULL << 20;
    fault(argv[2], "memory", ReadFailure::ResourceLimit, quick);
    fault(argv[2], "file-size", ReadFailure::ResourceLimit, quick);
    for (auto mode : {"crash", "truncated", "invalid-json", "duplicate-reply"})
        fault(argv[2], mode, ReadFailure::WorkerFailure, quick);
    fault(argv[2], "oversize", ReadFailure::ResourceLimit, quick);
    {
        RemoteSession session(QString::fromLocal8Bit(argv[2]));
        require(session.create("die-idle", Master), "idle-death control reply");
        QThread::msleep(30);
        require(session.groups().empty() && session.lastReadFailure() == ReadFailure::WorkerFailure
            && !session.processId(), "broken channel does not deliver SIGPIPE to controller");
    }
    {
        RemoteSession session(QString::fromLocal8Bit(argv[2]));
        require(session.create("normal", Master), "large request control starts");
        EntryDetails huge;
        huge.title.assign((16U << 20) + 1, 'x');
        require(session.addEntry({}, huge).empty() && session.lastReadFailure() == ReadFailure::ResourceLimit
            && !session.processId(), "oversized request refused before sending and worker discarded");
    }
    cancel(argv[2]);
    QTemporaryDir directory;
    const auto orphanPid = directory.filePath("orphan-pid");
    QProcess parent;
    parent.start(QCoreApplication::applicationFilePath(), {"--parent-driver", QString::fromLocal8Bit(argv[2]), orphanPid});
    require(parent.waitForStarted(), "parent-death driver starts");
    QElapsedTimer orphanWait; orphanWait.start();
    while (!QFile::exists(orphanPid) && orphanWait.elapsed() < 3000) QThread::msleep(5);
    QFile identity(orphanPid);
    require(identity.open(QIODevice::ReadOnly), "worker PID published for parent-death test");
    const auto orphan = identity.readAll().toLongLong();
    require(orphan > 0, "valid orphan PID");
    parent.kill(); require(parent.waitForFinished(1500), "parent driver terminates promptly");
    bool gone = false;
    while (orphanWait.elapsed() < 4500) {
        QFile stats(QStringLiteral("/proc/%1/stat").arg(orphan));
        if (!stats.open(QIODevice::ReadOnly)) { gone = true; break; }
        const auto bytes = stats.readAll();
        if (bytes.mid(bytes.lastIndexOf(')') + 2).startsWith('Z')) { gone = true; break; }
        QThread::msleep(5);
    }
    require(gone, "parent death leaves no running worker");
    std::puts("PASS parent-death cleanup");
    const auto fixture = makeLegacyFixture(KeePass2::CIPHER_AES256, false);
    auto input = fields(encrypt(fixture, compress(fixture, fixture.plaintext)));
    bool changed = false;
    for (auto& field : input.fields) {
        if (field.first == char(KeePass2::HeaderFieldID::TransformRounds)) {
            field.second = Endian::sizedIntToBytes<quint64>(100000000, KeePass2::BYTEORDER); changed = true;
        }
    }
    require(changed, "synthetic expensive KDF header");
    const auto filePath = directory.filePath("expensive.kdbx");
    QFile file(filePath);
    const auto bytes = encodeHeader(input);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write private KDF fixture"); file.close();
    cancel(argv[1], filePath.toStdString());
    RemoteSession stale(argv[1]);
    stale.beginTask(0); stale.cancel();
    require(!stale.unlock(filePath.toStdString(), Master) && stale.lastReadFailure() == ReadFailure::Cancelled
        && !stale.processId(), "cancelled queued task never starts a worker");
    require(file.open(QIODevice::ReadOnly) && file.readAll() == bytes, "cancellation leaves input unchanged");
    file.close();
    auto refusal = [&](const QByteArray& data, ReadFailure expected, const char* label) {
        QFile candidate(directory.filePath("refused.kdbx"));
        require(candidate.open(QIODevice::WriteOnly | QIODevice::Truncate) && candidate.write(data) == data.size(), "write refusal fixture");
        candidate.close();
        RemoteSession session(QString::fromLocal8Bit(argv[1]));
        require(!session.unlock(candidate.fileName().toStdString(), Master) && session.lastReadFailure() == expected,
                "isolated reader preserves admission failure classification");
        empty(session, directory);
        require(candidate.open(QIODevice::ReadOnly) && candidate.readAll() == data, "refused input unchanged");
        std::printf("PASS isolated reader refusal: %s\n", label);
    };
    const auto original = encrypt(fixture, compress(fixture, fixture.plaintext));
    refusal(original.left(original.size() - 1), ReadFailure::InvalidInput, "truncated encryption");
    auto budget = fields(original);
    for (auto& field : budget.fields)
        if (field.first == char(KeePass2::HeaderFieldID::TransformRounds))
            field.second = Endian::sizedIntToBytes<quint64>(100000001, KeePass2::BYTEORDER);
    refusal(encodeHeader(budget), ReadFailure::ResourceLimit, "KDF budget before calculation");
    auto xml = fixture.plaintext;
    xml.replace("<Meta>", "<Meta><Future>synthetic unpreservable data</Future>");
    refusal(encrypt(fixture, compress(fixture, xml)), ReadFailure::UnsupportedData, "unpreservable XML");
    const auto fifo = directory.filePath("unwritten.fifo");
    require(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "private FIFO fixture");
    RemoteSession io(QString::fromLocal8Bit(argv[1]));
    require(!io.unlock(fifo.toStdString(), Master) && io.lastReadFailure() == ReadFailure::IoError,
            "isolated reader rejects a FIFO without waiting for a writer");
    for (unsigned kind = 0; kind < 3; ++kind) {
        ProcessLimits invalid;
        if (kind == 0) ++invalid.addressBytes;
        if (kind == 1) invalid.cpuSeconds = 0;
        if (kind == 2) invalid.wallMilliseconds = 0;
        RemoteSession refused(QString::fromLocal8Bit(argv[1]), invalid);
        require(!refused.create("Synthetic", Master) && !refused.processId(), "no process limit widening or zero-budget bypass");
    }
    std::puts("PASS process resource limits, protocol failures, cancellation, reaping and restart");
}
