#pragma once

#include <atomic>

struct TStatistics {
    std::atomic<uint64_t> Spawned;
    std::atomic<uint64_t> Errors;
    std::atomic<uint64_t> Warns;
    std::atomic<uint64_t> MasterStarted;
    std::atomic<uint64_t> SlaveStarted;
    std::atomic<uint64_t> QueuedStatuses;
    std::atomic<uint64_t> QueuedEvents;
    std::atomic<uint64_t> ContainersCreated;
    std::atomic<uint64_t> ContainersStarted;
    std::atomic<uint64_t> RemoveDead;
    std::atomic<int> SlaveTimeoutMs;
    std::atomic<uint64_t> LogsRotated;
    std::atomic<uint64_t> RestoreFailed;
    std::atomic<uint64_t> EpollSources;
    std::atomic<uint64_t> ContainersCount;
    std::atomic<uint64_t> VolumesCount;
    std::atomic<uint64_t> ClientsCount;
    std::atomic<uint64_t> RequestsQueued;
    std::atomic<uint64_t> RequestsCompleted;
};

extern TStatistics *Statistics;
