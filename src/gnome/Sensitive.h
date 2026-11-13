#pragma once
#include "CoreSession.h"
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Erase application-owned, live values. Framework allocations, kernel buffers,
// a receiver's copy and compositor/history caches are outside this contract.
namespace Sensitive {
inline void erase(void* data, std::size_t size) { if (size) ::explicit_bzero(data, size); }
inline void wipe(std::string& value) { erase(value.data(), value.size()); value.clear(); }
inline void wipe(EntryDetails& value) {
    for (auto* field : {&value.id, &value.group, &value.title, &value.username, &value.password, &value.url, &value.notes}) wipe(*field);
}
inline void wipe(EntrySummary& value) { wipe(value.id); wipe(value.title); wipe(value.username); wipe(value.group); }
inline void wipe(GroupSummary& value) { wipe(value.id); wipe(value.name); }
template<class T> inline void wipe(T& value) { if constexpr (requires { value.clearSensitive(); }) value.clearSensitive(); }
template<class T> inline void wipe(std::vector<T>& values) { for (auto& value : values) wipe(value); values.clear(); }
template<class T> std::shared_ptr<T> owned(T* value) {
    return {value, [](T* p) { wipe(*p); delete p; }};
}
struct Erase {
    void* data;
    std::size_t size;
    ~Erase() { erase(data, size); }
};
}
