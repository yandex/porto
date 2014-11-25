#ifndef __PORTO_HPP__
#define __PORTO_HPP__

#include <string>
#include <atomic>

#include "version.hpp"

const std::string ROOT_CONTAINER = "/";
const std::string PORTO_ROOT_CGROUP = "porto";
const int REAP_EVT_FD = 128;
const int REAP_ACK_FD = 129;

struct TDaemonStat {
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

extern TDaemonStat *DaemonStat;

#define NO_COPY_CONSTRUCT(NAME) \
    NAME(const NAME &) = delete; \
    NAME &operator=(const NAME &) = delete

// TODO: rework this into some kind of notify interface
extern void AckExitStatus(int pid);

#include "config.hpp"

#endif
