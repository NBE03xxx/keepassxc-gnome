// Synthetic stream boundaries. The reference target links the unmodified core.
#include "crypto/Crypto.h"
#include "crypto/SymmetricCipher.h"
#include "streams/SymmetricCipherStream.h"
#include <QBuffer>
#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
namespace {
unsigned checks = 0;
void check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr,"FAIL stream finalization: %s\n",label); std::exit(1); }
    ++checks;
}
void run(SymmetricCipher::Mode mode, int size, bool split, bool reference) {
    const bool streamMode = mode == SymmetricCipher::ChaCha20 || mode == SymmetricCipher::Salsa20;
    const QByteArray key(32,'K'), iv(mode == SymmetricCipher::ChaCha20 ? 12 : mode == SymmetricCipher::Salsa20 ? 8 : 16,'I');
    QByteArray plain(size,'x'); for (int i=0;i<size;++i) plain[i]=char(1+i%127);
    auto expected=plain; SymmetricCipher direct;
    check(direct.init(mode,SymmetricCipher::Encrypt,key,iv),"direct cipher initialization");
    if (size) check(streamMode ? direct.process(expected) : direct.finish(expected),"upstream direct cipher output");
    QBuffer output; check(output.open(QIODevice::WriteOnly),"synthetic output");
    SymmetricCipherStream writer(&output);
    check(writer.init(mode,SymmetricCipher::Encrypt,key,iv) && writer.open(QIODevice::WriteOnly),"stream initialization");
    for (int offset=0;offset<size;) {
        const auto count=std::min(size-offset,split ? 137 : size);
        check(writer.write(plain.constData()+offset,count)==count,"complete write"); offset+=count;
    }
    const bool reset=writer.reset();
    const bool knownBug=reference && streamMode && size>0 && size%1024==0;
    check(reset != knownBug,knownBug ? "pristine upstream reproduces empty final-block failure" : "stream finalization succeeds");
    check(output.data()==expected,"ciphertext equals upstream direct cipher, including AES padding");
    writer.close();check(output.data()==expected,"close does not append ciphertext twice");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); check(Crypto::init(),"crypto initialization");
    const bool reference=argc==2 && QByteArray(argv[1])=="--reference";
    check(argc==1 || reference,"arguments");
    for (auto mode : {SymmetricCipher::ChaCha20,SymmetricCipher::Salsa20,SymmetricCipher::Aes256_CBC})
        for (int size : {0,1,15,16,17,1023,1024,1025,2047,2048,2049,4096})
            for (bool split : {false,true}) run(mode,size,split,reference);
    std::printf("PASS %s stream boundaries: %u checks\n",reference?"pristine-reference reproduction":"patched",checks);
}
