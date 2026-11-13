// Deliberately faulty worker, linked only into the regression tests.
#include "gnome/ProcessPolicy.h"
#include "gnome/SessionProtocol.h"
#include <QCoreApplication>
#include <atomic>
#include <vector>
#include <QTemporaryFile>

using namespace SessionProtocol;

int main(int argc, char** argv)
{
    ProcessLimits limits;
    if (!WorkerPolicy::initialize(argc, argv, limits)) return 78;
    try {
        QCoreApplication app(argc, argv);
        if (!send(1, {{"v", Version}, {"id", 0}, {"ready", true},
            {"addressBytes", double(limits.addressBytes)}, {"cpuSeconds", int(limits.cpuSeconds)}})) return 0;
        QJsonObject request;
        while (true) {
            if (!WorkerPolicy::armCpu(limits.cpuSeconds) || !receive(0, request)) return 0;
            const auto a = request["args"].toArray();
            const auto mode = a.isEmpty() ? QString{} : a[0].toString();
            if (mode == "wall") for (;;) ::pause();
            if (mode == "cpu") for (;;) std::atomic_signal_fence(std::memory_order_seq_cst);
            if (mode == "memory") {
                std::vector<QByteArray> blocks;
                for (;;) blocks.emplace_back(8 << 20, 'x');
            }
            if (mode == "file-size") {
                QTemporaryFile file;
                if (!file.open() || !QFile::remove(file.fileName())) return 70;
                const QByteArray block(1 << 20, 'x');
                for (;;) if (file.write(block) < 0 || !file.flush()) return 75;
            }
            if (mode == "crash") ::raise(SIGSEGV);
            if (mode == "oversize") {
                char prefix[4]; qToLittleEndian<quint32>(MaximumFrame + 1, prefix);
                transfer(1, prefix, sizeof(prefix), true);
                for (;;) ::pause();
            }
            if (mode == "truncated") {
                char prefix[4]; qToLittleEndian<quint32>(100, prefix);
                transfer(1, prefix, sizeof(prefix), true);
                return 0;
            }
            if (mode == "invalid-json") {
                char data[5]; qToLittleEndian<quint32>(1, data); data[4] = '?';
                transfer(1, data, sizeof(data), true);
                for (;;) ::pause();
            }
            if (mode == "duplicate-reply") {
                send(1, {{"v", Version}, {"id", 999}, {"failure", 0}, {"value", true}});
                for (;;) ::pause();
            }
            const auto value = (request["op"] == "create" || request["op"] == "enableRecovery") ? QJsonValue(true) : QJsonValue(QJsonArray{});
            if (!send(1, {{"v", Version}, {"id", request["id"]}, {"failure", 0}, {"value", value}})) return 0;
            if (mode == "die-idle") return 0;
        }
    } catch (const std::bad_alloc&) { _exit(75); }
    catch (...) { _exit(70); }
}
