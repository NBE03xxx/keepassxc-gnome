#pragma once
#include <gio/gio.h>
#include <functional>
#include <memory>
#include <cstdint>
#include <time.h>

class IdleLock {
public:
    static constexpr std::int64_t Timeout = 300000000; // five minutes, microseconds
    void activity(std::int64_t boot) { lastActivity = boot; }
    bool poll(std::int64_t boot, std::int64_t monotonic) {
        const bool resumed = previousBoot && boot - previousBoot - (monotonic - previousMonotonic) > 1000000;
        previousBoot = boot; previousMonotonic = monotonic;
        return resumed || (lastActivity && boot - lastActivity >= Timeout);
    }
    static std::int64_t bootTime() { timespec now{}; ::clock_gettime(CLOCK_BOOTTIME, &now); return now.tv_sec * 1000000LL + now.tv_nsec / 1000; }
private:
    std::int64_t lastActivity = 0, previousBoot = 0, previousMonotonic = 0;
};

// GLib main-context callbacks only; public desktop notifications, no global
// settings writes and no sleep/lock of the user's actual machine.
class SessionGuard {
public:
    SessionGuard(std::function<void()> lock, std::function<void(bool)> availability);
    ~SessionGuard();
    bool blocked() const;
    bool settled() const;
    void releaseSleepDelay();
    void refresh();
private:
    struct State;
    std::shared_ptr<State> state;
};
