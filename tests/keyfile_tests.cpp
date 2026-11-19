#include "CoreSession.h"
#include "KeyFileFixtures.h"
#include "gnome/RemoteSession.h"
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <type_traits>
#include <sys/stat.h>
#include <unistd.h>
using namespace KeyFileFixture;
namespace {
void reference(const QString& executable, const QStringList& arguments) {
    QProcess process; process.start(executable,arguments);
    check(process.waitForFinished(15000) && process.exitStatus()==QProcess::NormalExit && !process.exitCode(),"independent reference command");
}
template<class Session> void rejected(Session& session) {
    check(session.entries().empty() && session.groups().empty(),"failed unlock publishes no account model");
}
template<class Session> void exercise(Session& session, const QString& tool, const QString& source, const QString& keyPath,
                                     const QByteArray& bytes, const std::string& mode, const QString& output, const QString& recovery) {
    UnlockCredentials auth{mode!="none", mode=="password" ? Master : "",keyPath.toStdString()};
    check(session.unlock(source.toStdString(),auth),"matching factors unlock");
    const auto id=session.entries().front().id;
    if constexpr (std::is_same_v<Session,RemoteSession>) {
        QFile arguments(QStringLiteral("/proc/%1/cmdline").arg(session.processId()));
        check(arguments.open(QIODevice::ReadOnly),"read worker command line");
        const auto command=arguments.readAll();
        check(!command.contains(keyPath.toUtf8()) && !command.contains(Master),"key path and password never enter worker argv");
        check(QDir().mkdir(recovery) && !::chmod(QFile::encodeName(recovery),0700),"private recovery test folder");
        check(session.enableRecovery(recovery.toStdString()),"encrypted recovery enabled");
    }
    check(QFile::remove(keyPath),"remove key after unlock");
    check(session.rename(id,"Edited key file fixture") && session.saveCopy(output.toStdString()),"saved key remains usable without rereading key file");
    std::string checkpoint;
    if constexpr (std::is_same_v<Session,RemoteSession>) {
        checkpoint=session.lastCheckpoint(); check(!checkpoint.empty(),"mutation checkpoint retains composite key");
    }
    session.lock(); write(keyPath,bytes);
    reference(tool,{"verify",output,keyPath,QString::fromStdString(mode),"Edited key file fixture"});
    if (!checkpoint.empty()) reference(tool,{"verify",QString::fromStdString(checkpoint),keyPath,QString::fromStdString(mode),"Edited key file fixture"});
    check(session.unlock(output.toStdString(),auth),"saved file reopens with same factors");
    check(session.rename(id,"Overwrite key file fixture") && session.save(),"normal overwrite retains factors");
    const auto savedAs=output+"-as.kdbx";
    check(session.saveAs(savedAs.toStdString()),"save as retains factors"); session.lock();
    reference(tool,{"verify",output,keyPath,QString::fromStdString(mode),"Overwrite key file fixture"});
    reference(tool,{"verify",savedAs,keyPath,QString::fromStdString(mode),"Overwrite key file fixture"});
    auto wrong=auth; wrong.password="wrong synthetic password"; wrong.usePassword=true;
    check(!session.unlock(source.toStdString(),wrong),"wrong password factor refused"); rejected(session);
    wrong=auth; wrong.keyFile.clear();
    check(!session.unlock(source.toStdString(),wrong),"missing key factor refused"); rejected(session);
    wrong=auth; wrong.keyFile=keyPath.toStdString()+".missing";
    check(!session.unlock(source.toStdString(),wrong) && session.lastReadFailure()==ReadFailure::KeyFileIoError,"missing key file classified"); rejected(session);
    const auto bad=keyPath+".wrong"; write(bad,QByteArray(32,'z')); wrong=auth; wrong.keyFile=bad.toStdString();
    check(!session.unlock(source.toStdString(),wrong),"wrong key file refused"); rejected(session);
    if (mode=="none" || mode=="empty") {
        wrong=auth; wrong.usePassword=mode=="none"; wrong.password.clear();
        check(!session.unlock(source.toStdString(),wrong),"absent password differs from explicitly empty factor"); rejected(session);
    }
    auth.clearSensitive(); wrong.clearSensitive();
}
template<class Session> void badFiles(Session& session,const QString& source,const QString& path) {
    UnlockCredentials auth{false,{},path.toStdString()};
    write(path,{}); check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::InvalidKeyFile,"empty key rejected"); rejected(session);
    const auto xml=keyBytes(0); auto broken=xml; const auto offset=broken.indexOf("Hash=\""); check(offset>=0,"v2 hash present"); broken[offset+6]=broken[offset+6]=='0'?'1':'0';
    write(path,broken); check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::InvalidKeyFile,"XML v2 checksum rejected without hash fallback");
    write(path,"<KeyFile><Meta><Version>9.0</Version></Meta><Key><Data>abc</Data></Key></KeyFile>");
    check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::InvalidKeyFile,"unsupported key XML version rejected");
    QFile large(path); check(large.open(QIODevice::WriteOnly|QIODevice::Truncate) && large.resize((16LL<<20)+1),"oversized sparse key"); large.close();
    check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::KeyFileTooLarge,"oversized key rejected before parsing");
    check(QFile::remove(path) && !::mkfifo(path.toLocal8Bit().constData(),0600),"synthetic key FIFO");
    QElapsedTimer timer; timer.start(); check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::KeyFileIoError && timer.elapsed()<2000,"key FIFO cannot block");
    check(QFile::remove(path),"remove key FIFO"); auth.keyFile=QFileInfo(path).absolutePath().toStdString();
    check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::KeyFileIoError,"directory cannot be key file");
    write(path,keyBytes(2)); auth.keyFile=path.toStdString();
    check(!::chmod(QFile::encodeName(path),0000),"deny key permissions");
    check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::KeyFileIoError,"unreadable key rejected");
    check(!::chmod(QFile::encodeName(path),0600),"restore key permissions");
    auth.keyFile=path.toStdString()+std::string(1,'\0')+".suffix";
    check(!session.unlock(source.toStdString(),auth) && session.lastReadFailure()==ReadFailure::KeyFileIoError,"embedded NUL cannot truncate key path");
    auth={false,{}, {}}; check(!session.unlock(source.toStdString(),auth),"no credential factors rejected"); rejected(session);
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); check(argc==3 && CoreSession::initialize(),"key tests initialized"); QTemporaryDir temporary; check(temporary.isValid(),"synthetic folder");
    unsigned cases=0; const QString tool=QString::fromLocal8Bit(argv[1]);
    for (int format=0;format<5;++format) for (const auto version:{KeePass2::FILE_VERSION_3_1,KeePass2::FILE_VERSION_4,KeePass2::FILE_VERSION_4_1})
        for (const std::string mode:{"none","password","empty"}) {
            const auto prefix=temporary.filePath(QString::number(cases++)); const auto path=prefix+".keyx", source=prefix+".kdbx"; const auto bytes=keyBytes(format); write(path,bytes);
            reference(tool,{"create",source,path,QString::fromStdString(mode),QString::number(version,16)});
            CoreSession direct; exercise(direct,tool,source,path,bytes,mode,prefix+"-direct.kdbx",{});
            RemoteSession remote(QString::fromLocal8Bit(argv[2])); exercise(remote,tool,source,path,bytes,mode,prefix+"-remote.kdbx",prefix+"-recovery");
        }
    const auto path=temporary.filePath("bad.keyx"), source=temporary.filePath("limits.kdbx"); write(path,keyBytes(2));
    reference(tool,{"create",source,path,"none",QString::number(KeePass2::FILE_VERSION_4_1,16)});
    CoreSession direct; RemoteSession remote(QString::fromLocal8Bit(argv[2])); badFiles(direct,source,path); badFiles(remote,source,path);
    write(path,keyBytes(2)); ReadLimits limits; limits.keyFileBytes=31;
    check(!direct.unlock(source.toStdString(),UnlockCredentials{false,{},path.toStdString()},limits) && direct.lastReadFailure()==ReadFailure::KeyFileTooLarge,"smaller test budget enforced");
    check(direct.unlock(source.toStdString(),UnlockCredentials{false,{},path.toStdString()}),"fresh unlock works after input failures");
    direct.lock(); const auto passwordOnly=temporary.filePath("password-only.kdbx");
    check(CoreSession::createFixture(passwordOnly.toStdString(),FixtureFormat::Kdbx3),"password-only control");
    check(!direct.unlock(passwordOnly.toStdString(),UnlockCredentials{true,CoreSession::FixturePassword,path.toStdString()+".missing"}) && direct.lastReadFailure()==ReadFailure::KeyFileIoError,"selected unreadable key cannot silently fall back to correct password");
    std::printf("PASS key file: %u formats/versions/factor combinations, direct and isolated worker, pristine-core roundtrips, recovery and failure cases\n",cases);
}
