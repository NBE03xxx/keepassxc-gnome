#pragma once

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

struct ProcessLimits
{
    std::uint64_t addressBytes = 1ULL << 30;
    unsigned cpuSeconds = 15;
    unsigned wallMilliseconds = 30000;
    // Save performs both the upstream writer and a full upstream reader.
    unsigned saveCpuSeconds() const { return cpuSeconds * 3; }
    unsigned saveWallMilliseconds() const { return wallMilliseconds * 3; }

    bool valid() const
    {
        const ProcessLimits maximum;
        return addressBytes >= (32ULL << 20) && addressBytes <= maximum.addressBytes
            && cpuSeconds && cpuSeconds <= maximum.cpuSeconds
            && wallMilliseconds && wallMilliseconds <= maximum.wallMilliseconds;
    }
};

namespace WorkerPolicy {
inline bool armCpu(unsigned seconds)
{
    rusage usage{};
    rlimit limit{};
    if (getrusage(RUSAGE_SELF, &usage) || getrlimit(RLIMIT_CPU, &limit)) return false;
    const auto micros = std::uint64_t(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000
        + usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
    // The kernel limit is cumulative and has one-second granularity. Keep the
    // inherited hard ceiling; never consume an entire lifetime budget at idle.
    limit.rlim_cur = (micros + 999999) / 1000000 + seconds;
    return limit.rlim_cur <= limit.rlim_max && setrlimit(RLIMIT_CPU, &limit) == 0;
}

inline bool initialize(int argc, char** argv, ProcessLimits& limits)
{
    if (argc != 4) return false;
    auto number = [](const char* s, std::uint64_t& value) {
        if (!*s || *s < '0' || *s > '9') return false;
        char* end = nullptr;
        errno = 0;
        value = std::strtoull(s, &end, 10);
        return !errno && end && !*end;
    };
    std::uint64_t memory, cpu, parent;
    if (!number(argv[1], memory) || !number(argv[2], cpu) || !number(argv[3], parent)
        || cpu > ProcessLimits{}.cpuSeconds || !parent) return false;
    limits.addressBytes = memory;
    limits.cpuSeconds = cpu;
    if (!limits.valid()) return false;
    const rlimit core{0, 0};
    const rlimit address{memory, memory};
    const rlimit fileBytes{128ULL << 20, 128ULL << 20};
    if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_AS, &address)
        || setrlimit(RLIMIT_FSIZE, &fileBytes)
        || prctl(PR_SET_DUMPABLE, 0) || prctl(PR_SET_PDEATHSIG, SIGKILL)
        || std::uint64_t(getppid()) != parent || !armCpu(limits.cpuSeconds)) return false;
    // stdin/stdout refer to one private socket endpoint. Keep the parent's
    // endpoint nonblocking, but let the isolated worker wait for requests.
    const auto flags = fcntl(STDIN_FILENO, F_GETFL);
    return flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) == 0;
}
}
