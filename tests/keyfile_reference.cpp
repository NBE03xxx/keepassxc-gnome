#include "KeyFileFixtures.h"
#include "crypto/Crypto.h"
#include <QCoreApplication>
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv); KeyFileFixture::check(Crypto::init(),"reference crypto");
    KeyFileFixture::check(argc==6,"reference arguments");
    if (std::string(argv[1])=="create") KeyFileFixture::create(argv[2],argv[3],argv[4],QString(argv[5]).toUInt(nullptr,16));
    else if (std::string(argv[1])=="verify") KeyFileFixture::verify(argv[2],argv[3],argv[4],argv[5]);
    else return 2;
    return 0;
}
