#pragma once

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

class Entry;

struct AutoTypeStep
{
    enum class Kind { Character, Tab, Enter, Delay };
    Kind kind;
    std::uint32_t value = 0;
    bool operator==(const AutoTypeStep&) const = default;
};

struct AutoTypeSequence
{
    enum class Status { Unavailable, Unsupported, Ready };
    static constexpr unsigned MaximumSteps = 512;
    static constexpr unsigned MaximumTotalDelayMs = 5000;

    AutoTypeSequence() = default;
    AutoTypeSequence(const AutoTypeSequence&) = delete;
    AutoTypeSequence& operator=(const AutoTypeSequence&) = delete;
    AutoTypeSequence(AutoTypeSequence&& other) noexcept
        : status(std::exchange(other.status, Status::Unavailable)), steps(std::move(other.steps))
    {
    }
    AutoTypeSequence& operator=(AutoTypeSequence&& other) noexcept
    {
        if (this != &other) {
            clearSensitive();
            status = std::exchange(other.status, Status::Unavailable);
            steps = std::move(other.steps);
        }
        return *this;
    }
    ~AutoTypeSequence() { clearSensitive(); }

    void clearSensitive()
    {
        if (!steps.empty()) ::explicit_bzero(steps.data(), steps.size() * sizeof(AutoTypeStep));
        steps.clear();
        status = Status::Unavailable;
    }

    Status status = Status::Unavailable;
    std::vector<AutoTypeStep> steps;
};

AutoTypeSequence selectedEntryAutoType(const Entry& entry);
