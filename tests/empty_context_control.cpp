// Structural counterexample for a zero-payload allocation, as used by Mesa
// ralloc_context: the owned pointer is exactly one past the allocation.
// This is a minimal model, not a replacement implementation of Mesa ralloc.
#include <sanitizer/lsan_interface.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
void* owner = nullptr;
void* canonicalRoot = nullptr;
int main() {
    std::thread([] {
        auto* base = static_cast<char*>(std::malloc(48));
        if (!base) std::abort();
        std::memset(base, 0, 48);
        owner = base + 48;
    }).join();
    const int owned = __lsan_do_recoverable_leak_check();
    std::printf("OWNED ONE-PAST-END CONTEXT: lsan=%d\n", owned);
    canonicalRoot = static_cast<char*>(owner) - 48;
    const int rooted = __lsan_do_recoverable_leak_check();
    std::printf("SAME CONTEXT WITH CANONICAL ROOT: lsan=%d\n", rooted);
    std::free(canonicalRoot); owner = canonicalRoot = nullptr;
    const int freed = __lsan_do_recoverable_leak_check();
    std::printf("DESTROYED CONTEXT: lsan=%d\n", freed);
    std::fflush(stdout);
    return owned == 1 && rooted == 0 && freed == 0 ? 0 : 3;
}
