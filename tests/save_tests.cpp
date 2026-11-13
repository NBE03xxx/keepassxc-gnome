// Synthetic databases only. Filesystem fault injection is test-only C++ DI.
#include "ReaderFixtures.h"
#include "gnome/SafeSave.h"
#include "gnome/RemoteSession.h"
#include "gnome/SessionProtocol.h"
#include "format/KeePass2Reader.h"
#include <QDir>
#include <QCryptographicHash>
#include <QProcess>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <type_traits>

using namespace ReaderTest;
using namespace SafeSave;
namespace {
unsigned checks = 0;
void check(bool ok, const char* label) { require(ok, label); ++checks; }
QByteArray load(const QString& path) { QFile f(path); require(f.open(QIODevice::ReadOnly), "read test file"); return f.readAll(); }
void put(const QString& path, const QByteArray& data) { QFile f(path); require(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size(), "write test file"); }
FileStamp identify(const QString& path) { int fd = ::open(QFile::encodeName(path), O_RDONLY | O_NOFOLLOW | O_CLOEXEC); auto result = fingerprint(fd); if (fd >= 0) ::close(fd); return result; }
bool verify(QFile& file) { Database db; KeePass2Reader reader; return file.seek(0) && reader.readDatabase(&file, key(), &db); }
bool readable(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) && verify(file); }
SaveResult save(const QString& path, const FileStamp* expected, const QByteArray& payload, const Observer& observer = {})
{
    return SafeSave::write(path, expected, [&](QFile& file) { return file.write(payload) == payload.size(); }, verify, observer);
}
std::vector<RecoveryRecord> list(const QString& path) { bool complete; auto rows = recoveries(path, complete); check(complete, "bounded recovery listing completes"); return rows; }
void protect(const QString& path, unsigned mode) { require(!::chmod(QFile::encodeName(path), mode), "chmod test file"); }
void permissions(const QString& path, unsigned mode) { struct stat s{}; check(!::lstat(QFile::encodeName(path), &s) && (s.st_mode & 0777) == mode, "private filesystem permissions"); }
void checkCopies(const QString& folder, const QByteArray& original, const QByteArray& payload, bool wantedOriginal)
{
    auto records = list(folder);
    bool old = false, newer = false;
    for (const auto& record : records) {
        permissions(QString::fromStdString(record.directory), 0700);
        for (const auto& file : record.files) {
            auto data = load(QString::fromStdString(file.path));
            if (data == original) old = true;
            if (data == payload) newer = true;
            if (file.kind == "written.kdbx" || file.kind == "previous.kdbx") permissions(QString::fromStdString(file.path), 0600);
            // A journal hash is an aid, never a substitute for KDBX authentication.
            if (file.hashMatches) check(readable(QString::fromStdString(file.path)), "recorded complete copy reads with upstream reader");
        }
    }
    if (wantedOriginal) check(old, "independent original backup is recoverable");
    check(newer, "new encrypted version is recoverable");
}

void filesystem(const QByteArray& original, const QByteArray& payload)
{
    {
        QTemporaryDir d; const auto target = d.filePath("test.kdbx");
        auto result = save(target, nullptr, payload);
        check(result.committed() && result.saved.valid() && load(target) == payload, "new file commits durably");
        permissions(target, 0600); checkCopies(d.path(), original, payload, false);
        auto before = load(target);
        check(save(target, nullptr, original).status == SaveStatus::Exists && load(target) == before, "Save As never overwrites existing file");
        auto stamp = identify(target);
        result = save(target, &stamp, original);
        check(result.committed() && load(target) == original, "normal save exchanges complete encrypted files");
        checkCopies(d.path(), payload, original, true);
        auto records = list(d.path());
        for (const auto& record : records) check(discard(QString::fromStdString(record.directory)), "explicit recovery cleanup succeeds");
        check(list(d.path()).empty() && load(target) == original, "cleanup retains live database");
    }
    for (auto stage : {Stage::Prepared, Stage::BackupSync, Stage::Written, Stage::FileSync, Stage::Verified, Stage::Ready, Stage::FinalChecked, Stage::Published, Stage::DirectorySync, Stage::Complete}) {
        QTemporaryDir d; const auto target = d.filePath("test.kdbx"); put(target, original); auto stamp = identify(target);
        auto result = save(target, &stamp, payload, [stage](Stage point) { if (point == stage) { errno = ENOSPC; return false; } return true; });
        check(!result.committed() && !result.durable, "injected failure never claims durable success");
        const bool published = int(stage) >= int(Stage::Published);
        check(result.published == published && load(target) == (published ? payload : original), "failure preserves old file until publication");
        if (published) { check(result.status == SaveStatus::Uncertain, "post-publication error is uncertain"); checkCopies(d.path(), original, payload, true); }
    }
    {
        QTemporaryDir d; const auto target = d.filePath("test.kdbx"); put(target, original); auto stamp = identify(target);
        auto result = SafeSave::write(target, &stamp, [](QFile& file) { file.write("truncated", 9); return false; }, verify);
        check(result.status == SaveStatus::SerializationError && load(target) == original, "partial writer output never replaces database");
        result = SafeSave::write(target, &stamp, [](QFile& file) { return file.write("invalid encrypted output") > 0; }, verify);
        check(result.status == SaveStatus::VerificationError && load(target) == original, "invalid KDBX is rejected before publication");
    }
    {
        QTemporaryDir d; const auto target = d.filePath("test.kdbx"), link = d.filePath("link.kdbx");
        check(!::symlink("missing", QFile::encodeName(link)), "dangling symlink setup");
        check(save(link, nullptr, payload).status == SaveStatus::Exists && QFileInfo(link).isSymLink(), "dangling symlink is not overwritten");
        put(target, original); auto stamp = identify(target);
        check(!::link(QFile::encodeName(target), QFile::encodeName(d.filePath("hard.kdbx"))), "hardlink setup");
        check(save(target, &stamp, payload).status == SaveStatus::UnsafePath && load(target) == original, "hardlinked original refused");
        QFile::remove(d.filePath("hard.kdbx")); protect(target, 0400);
        stamp = identify(target);
        check(save(target, &stamp, payload).status == SaveStatus::UnsafePath && load(target) == original, "read-only original refused");
        protect(target, 0600);
        QFile::remove(link); check(!::symlink(QFile::encodeName(target), QFile::encodeName(link)), "source symlink setup");
        stamp = identify(target);
        check(save(link, &stamp, payload).status == SaveStatus::UnsafePath && load(target) == original, "source symlink refused for replacement");
    }
    {
        QTemporaryDir d; const auto target = d.filePath("deleted.kdbx"); put(target, original); auto stamp = identify(target);
        require(QFile::remove(target), "external deletion setup");
        check(save(target, &stamp, payload).status == SaveStatus::Conflict && !QFile::exists(target), "externally deleted source is not recreated by normal save");
    }
    for (bool late : {false, true}) {
        QTemporaryDir d; const auto target = d.filePath("test.kdbx"); put(target, original); auto stamp = identify(target);
        const auto external = payload + QByteArray("external-change");
        auto result = save(target, &stamp, payload, [&](Stage point) {
            if (point == (late ? Stage::FinalChecked : Stage::Ready)) put(target, external);
            return true;
        });
        check(result.status == SaveStatus::Conflict && !result.committed(), "external update is reported as a conflict");
        check(load(target) == (late ? payload : external), "pre-publication conflict does not overwrite external data");
        auto records = list(d.path()); bool retained = !late;
        for (auto& record : records) for (auto& file : record.files)
            if (load(QString::fromStdString(file.path)) == external) retained = true;
        check(retained, "late external update retained as displaced inode");
        checkCopies(d.path(), original, payload, true);
    }
    {
        QTemporaryDir root; const auto folder = root.filePath("db"); require(QDir().mkdir(folder), "parent setup");
        auto target = folder + "/test.kdbx"; put(target, original); auto stamp = identify(target);
        auto result = save(target, &stamp, payload, [&](Stage point) { if (point == Stage::Ready) { require(QDir().rename(folder, folder + "-moved") && QDir().mkdir(folder), "parent race"); } return true; });
        check(result.status == SaveStatus::UnsafePath && !result.published && !QFile::exists(target)
            && load(folder + "-moved/test.kdbx") == original, "renamed parent path is detected before publication");
    }
    {
        QTemporaryDir d; auto target = d.filePath("test.kdbx"); put(target, original); auto stamp = identify(target);
        SaveResult nested;
        auto result = save(target, &stamp, payload, [&](Stage point) { if (point == Stage::Ready) nested = save(target, &stamp, original); return true; });
        check(result.committed() && nested.status == SaveStatus::Busy, "cooperative saves serialize with target lock");
    }
    {
        QTemporaryDir d; auto target = d.filePath("test.kdbx"); put(target, original); auto stamp = identify(target);
        auto result = save(target, &stamp, payload); check(result.committed(), "cleanup tamper setup");
        protect(QString::fromStdString(result.recovery), 0755);
        bool complete;
        check(recoveries(d.path(), complete).empty() && !complete, "unsafe recovery permissions do not claim complete empty scan");
        protect(QString::fromStdString(result.recovery), 0700);
        check(recoveries(d.path(), complete).size() == 1 && complete, "safe recovery permissions restore complete scan");
        put(QString::fromStdString(result.recovery) + "/foreign.txt", "keep me");
        check(!discard(QString::fromStdString(result.recovery)) && load(target) == payload, "cleanup refuses unexpected children");
        QFile::remove(QString::fromStdString(result.recovery) + "/foreign.txt");
        put(QString::fromStdString(result.recovery) + "/written.kdbx", "changed");
        bool badHash = false;
        for (const auto& record : list(d.path())) for (const auto& f : record.files)
            if (f.kind == "written.kdbx") badHash = !f.hashMatches;
        check(badHash, "recovery listing reports changed hash");
        check(!discard(d.path()) && QFile::exists(target), "cleanup refuses arbitrary directory");
        check(discard(QString::fromStdString(result.recovery)) && load(target) == payload, "explicit cleanup permits unverified own copies");
    }
    {
        QTemporaryDir d; auto target = d.filePath("test.kdbx");
        // Incomplete records also consume retention budget; no silent deletion.
        const auto prefix = QByteArray(Prefix) + QCryptographicHash::hash(QByteArray("test.kdbx"), QCryptographicHash::Sha256).toHex().left(16) + '-';
        for (int i = 0; i < 64; ++i) require(QDir().mkdir(d.filePath(QString::fromLatin1(prefix) + QString::number(i).rightJustified(32, '0'))), "retention setup");
        check(save(target, nullptr, payload).status == SaveStatus::RecoveryFull && !QFile::exists(target), "retention cap refuses further saves without deleting backups");
    }
}

template<class Session> void lifecycle(const QString& worker, const QByteArray& original)
{
    std::unique_ptr<Session> owned;
    if constexpr (std::is_same_v<Session, RemoteSession>) owned = std::make_unique<Session>(worker);
    else owned = std::make_unique<Session>();
    auto& session = *owned;
    QTemporaryDir d; const auto target = d.filePath("test.kdbx"), copy = d.filePath("copy.kdbx"), next = d.filePath("new.kdbx"); put(target, original);
    check(session.unlock(target.toStdString(), Master) && session.hasSource() && !session.isModified(), "opened baseline supports normal save");
    auto entry = session.entries().front().id;
    check(session.rename(entry, "Synthetic saved title") && session.isModified(), "edit sets dirty state");
    check(session.saveCopy(copy.toStdString()) && session.isModified() && load(target) == original, "save copy preserves dirty state and active source");
    check(session.save() && session.lastSaveResult().committed() && !session.isModified() && load(target) != original, "normal save clears dirty only after durable commit");
    auto current = load(target);
    const auto nextRenamed = session.rename(entry, "Synthetic next title"), associated = nextRenamed && session.saveAs(next.toStdString());
    if (!associated || session.isModified() || load(target) != current)
        std::fprintf(stderr, "Save As diagnostic: remote=%d renamed=%d saved=%d modified=%d originalUnchanged=%d status=%u\n", int(std::is_same_v<Session, RemoteSession>), int(nextRenamed), int(associated), int(session.isModified()), int(load(target) == current), unsigned(session.lastSaveResult().status));
    check(associated && !session.isModified() && load(target) == current, "Save As associates new file without changing old source");
    const auto currentRenamed = session.rename(entry, "Synthetic current title"), savedCurrent = currentRenamed && session.save();
    if (!savedCurrent || load(target) != current)
        std::fprintf(stderr, "Normal-save diagnostic: remote=%d renamed=%d saved=%d originalUnchanged=%d status=%u\n", int(std::is_same_v<Session, RemoteSession>), int(currentRenamed), int(savedCurrent), int(load(target) == current), unsigned(session.lastSaveResult().status));
    check(savedCurrent && load(target) == current, "next normal save follows Save As association");
    auto external = load(next) + QByteArray("external-change"); put(next, external);
    check(session.rename(entry, "Synthetic conflicting title") && !session.save() && session.isModified()
        && session.lastSaveResult().status == SaveStatus::Conflict && load(next) == external, "external updates refuse overwrite and retain edits");
    const auto safe = d.filePath("after-conflict.kdbx");
    check(session.saveAs(safe.toStdString()) && !session.isModified() && load(next) == external, "conflicting edits can be saved under new filename");
    session.lock(); check(!session.hasSource() && !session.save(), "locked session cannot save");
    auto records = list(d.path()); check(!records.empty(), "normal save retains recovery records");
    std::string recovery;
    for (const auto& record : records) for (const auto& f : record.files) if (f.kind == "previous.kdbx") recovery = f.path;
    check(!recovery.empty() && session.unlock(recovery, Master) && !session.hasSource(), "recovery opens without overwrite association");
    const auto encrypted = load(QString::fromStdString(recovery));
    check(!session.save() && session.lastSaveResult().status == SaveStatus::NoSource && load(QString::fromStdString(recovery)) == encrypted, "recovery record cannot be overwritten by normal save");
    check(session.saveAs(d.filePath("recovered.kdbx").toStdString()), "recovered model saves under a new filename");
    session.lock();
}

int lostReplyWorker(int argc, char** argv)
{
    ProcessLimits limits;
    require(WorkerPolicy::initialize(argc, argv, limits), "test worker policy");
    using namespace SessionProtocol;
    CoreSession session;
    send(1, {{"v", Version}, {"id", 0}, {"ready", true}, {"addressBytes", double(limits.addressBytes)}, {"cpuSeconds", int(limits.cpuSeconds)}});
    QJsonObject request;
    while (receive(0, request)) {
        const auto op = request["op"].toString(); const auto a = request["args"].toArray(); QJsonValue value;
        if (op == "create") value = session.create(text(a[0]), text(a[1]));
        else if (op == "saveAs") { require(WorkerPolicy::armCpu(limits.saveCpuSeconds()), "save CPU budget"); require(session.saveAs(text(a[0])), "test worker saves real database"); _exit(0); }
        else if (op == "recoveries") { bool complete; QJsonArray records; for (auto& r : recoveries(string(a[0]), complete)) records.append(encode(r)); value = QJsonObject{{"complete", complete}, {"records", records}}; }
        else throw Error{};
        if (!send(1, {{"v", Version}, {"id", request["id"]}, {"failure", 0}, {"value", value}})) return 0;
    }
    return 0;
}
}

int main(int argc, char** argv)
{
    ::setenv("BOTAN_THREAD_POOL_SIZE", "2", 1);
    QCoreApplication app(argc, argv); require(CoreSession::initialize(), "crypto initialization");
    if (argc == 4 && argv[1][0] >= '0' && argv[1][0] <= '9') return lostReplyWorker(argc, argv);
    if (argc == 6 && QByteArray(argv[1]) == "--crash") {
        const auto path = QString::fromLocal8Bit(argv[2]); auto stamp = identify(path);
        auto payload = load(QString::fromLocal8Bit(argv[4])); const auto stage = Stage(QByteArray(argv[3]).toInt());
        save(path, QByteArray(argv[5]) == "replace" ? &stamp : nullptr, payload, [stage](Stage point) { if (point == stage) ::kill(::getpid(), SIGKILL); return true; });
        return 3;
    }
    const bool memory = argc == 3 && QByteArray(argv[2]) == "--memory";
    require(argc == 2 || memory, "real worker argument");
    auto a = makeFixture(KeePass2::FILE_VERSION_4_1, KeePass2::CIPHER_CHACHA20, true);
    auto b = makeFixture(KeePass2::FILE_VERSION_4_1, KeePass2::CIPHER_CHACHA20, true, KeePass2::KDF_AES_KDBX4,
        [](Database&, Entry& e) { e.setTitle("Synthetic new version"); });
    const auto original = encrypt(a, compress(a, a.plaintext)), payload = encrypt(b, compress(b, b.plaintext));
    filesystem(original, payload);
    lifecycle<CoreSession>(argv[1], original);
    lifecycle<RemoteSession>(argv[1], original);
    {
        QTemporaryDir d;
        const auto target = d.filePath("test.kdbx"); put(target, original); auto stamp = identify(target);
        const auto saved = save(target, &stamp, payload);
        const auto backup = saved.recovery + "/previous.kdbx";
        RemoteSession session(QString::fromLocal8Bit(argv[1]));
        check(session.unlock(backup, Master), "active recovery protection setup");
        bool complete; auto records = session.recoveries(d.path().toStdString(), complete);
        check(complete && records.size() == 1 && !session.entries().empty(), "recovery scan does not replace live model");
        check(!session.discardRecovery(saved.recovery) && QFile::exists(QString::fromStdString(backup)), "active recovery source cannot be deleted");
        check(!session.saveCopy(saved.recovery + "/another.kdbx") && session.lastSaveResult().status == SaveStatus::UnsafePath,
            "saving inside recovery record is refused");
        check(session.saveAs(d.filePath("rescued.kdbx").toStdString()) && session.discardRecovery(saved.recovery), "Save As releases active recovery protection");
        check(!session.entries().empty(), "cleanup never discards live database model");
    }
    if (memory) { std::printf("PASS instrumented save/recovery: %u checks, raw and isolated lifecycle\n", checks); return 0; }
    QTemporaryDir input; const auto newFile = input.filePath("new.kdbx"); put(newFile, payload);
    for (bool replace : {false, true}) for (auto stage : {Stage::Prepared, Stage::Written, Stage::FileSync, Stage::Verified, Stage::Ready, Stage::FinalChecked, Stage::Published, Stage::DirectorySync, Stage::Complete}) {
        QTemporaryDir d; const auto path = d.filePath("test.kdbx"); if (replace) put(path, original);
        QProcess child; child.start(QCoreApplication::applicationFilePath(), {"--crash", path, QString::number(int(stage)), newFile, replace ? "replace" : "new"});
        check(child.waitForStarted() && child.waitForFinished(10000) && child.exitStatus() == QProcess::CrashExit, "crash driver killed at transaction boundary");
        const bool published = int(stage) >= int(Stage::Published);
        if (published) check(load(path) == payload && readable(path), "published file remains complete after process death");
        else if (replace) check(load(path) == original, "original byte-identical after early process death");
        else check(!QFile::exists(path), "new target absent after early process death");
        if (int(stage) >= int(Stage::Ready)) checkCopies(d.path(), original, payload, replace);
    }
    {
        QTemporaryDir d; const auto target = d.filePath("lost-reply.kdbx");
        RemoteSession session(QCoreApplication::applicationFilePath());
        check(session.create("Synthetic lost reply", Master), "lost reply worker starts");
        check(!session.saveAs(target.toStdString()) && session.lastSaveResult().status == SaveStatus::Uncertain
            && QFile::exists(target), "lost acknowledgment never claims save failure left destination unchanged");
        bool complete; auto records = session.recoveries(d.path().toStdString(), complete);
        check(complete && records.size() == 1 && records.front().state == "committed", "durable recovery journal resolves lost acknowledgment");
        CoreSession read; check(read.unlock(target.toStdString(), Master), "lost acknowledgment still leaves valid KDBX");
    }
    std::printf("PASS safe save/recovery: %u checks, 18 process-death boundaries, raw and isolated lifecycle\n", checks);
}
