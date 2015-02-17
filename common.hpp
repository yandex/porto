#pragma once

#include <atomic>

#include "error.hpp"
#include "version.hpp"

#define PORTO_ASSERT(EXPR) \
    do { \
        if (!(EXPR)) { \
            L_ERR() << "Assertion failed: " << # EXPR << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            Crash(); \
        } \
    } while (0)

#define PORTO_RUNTIME_ERROR(MSG) \
    do { \
        L_ERR() << "Runtime error: " << (MSG) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        Crash(); \
    } while (0)

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
    std::atomic<uint64_t> Started;
    std::atomic<uint64_t> RemoveDead;
    std::atomic<int> SlaveTimeoutMs;
    std::atomic<uint64_t> Rotated;
    std::atomic<uint64_t> RestoreFailed;
};

extern TStatistics *Statistics;
