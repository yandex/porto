#pragma once

#include <atomic>
#include <mutex>

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
    TNonCopyable(TNonCopyable const&&) = delete;
    TNonCopyable& operator= (TNonCopyable const&&) = delete;
};

class TLockable {
protected:
    std::mutex Mutex;
public:
    std::unique_lock<std::mutex> ScopedLock() { return std::unique_lock<std::mutex>(Mutex); }
};

const std::string ROOT_CONTAINER = "/";
constexpr uint16_t ROOT_CONTAINER_ID = 1;
const std::string DOT_CONTAINER = ".";
constexpr uint16_t PORTO_ROOT_CONTAINER_ID = 3;
const std::string PORTO_ROOT_CONTAINER = "/porto";
const std::string PORTO_ROOT_CGROUP = "porto";
const std::string PORTO_DAEMON_CGROUP = "portod";

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
    std::atomic<uint64_t> InterruptedReads;
    std::atomic<uint64_t> EpollSources;
};

extern TStatistics *Statistics;

extern void AckExitStatus(int pid);
