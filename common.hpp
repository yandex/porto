#pragma once

#include <atomic>

#include "error.hpp"
#include "version.hpp"

class TNonCopyable {
protected:
    TNonCopyable() = default;
    ~TNonCopyable() = default;
private:
    TNonCopyable(TNonCopyable const&) = delete;
    TNonCopyable& operator= (TNonCopyable const&) = delete;
};

const std::string ROOT_CONTAINER = "/";
const std::string PORTO_ROOT_CGROUP = "porto";

struct TStatistics {
    std::atomic<uint64_t> Spawned;
    std::atomic<uint64_t> Errors;
    std::atomic<uint64_t> Warns;
    std::atomic<uint64_t> MasterStarted;
    std::atomic<uint64_t> SlaveStarted;
    std::atomic<uint64_t> QueuedStatuses;
    std::atomic<uint64_t> QueuedEvents;
    std::atomic<uint64_t> Created;
    std::atomic<uint64_t> RemoveDead;
    std::atomic<int> SlaveTimeoutMs;
    std::atomic<uint64_t> Rotated;
    std::atomic<uint64_t> RestoreFailed;
};

extern TStatistics *Statistics;
