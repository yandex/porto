#pragma once

#include <atomic>

struct TStatistics {
    std::atomic<uint64_t> Spawned;
    std::atomic<uint64_t> Errors;
    std::atomic<uint64_t> Warns;
    std::atomic<uint64_t> MasterStarted;
    std::atomic<uint64_t> PortoStarted;
    std::atomic<uint64_t> QueuedStatuses;
    std::atomic<uint64_t> QueuedEvents;
    std::atomic<uint64_t> ContainersCreated;
    std::atomic<uint64_t> ContainersStarted;
    std::atomic<uint64_t> ContainersFailedStart;
    std::atomic<uint64_t> ContainersOOM;
    std::atomic<uint64_t> RemoveDead;
    std::atomic<uint64_t> LogLines;
    std::atomic<uint64_t> LogBytes;
    std::atomic<uint64_t> LogRotateBytes;
    std::atomic<uint64_t> LogRotateErrors;
    std::atomic<uint64_t> RestoreFailed;
    std::atomic<uint64_t> EpollSources;
    std::atomic<uint64_t> ContainersCount;
    std::atomic<uint64_t> VolumesCount;
    std::atomic<uint64_t> ClientsCount;
    std::atomic<uint64_t> RequestsQueued;
    std::atomic<uint64_t> RequestsCompleted;
    std::atomic<uint64_t> RequestsLonger1s;
    std::atomic<uint64_t> RequestsLonger3s;
    std::atomic<uint64_t> RequestsLonger30s;
    std::atomic<uint64_t> RequestsLonger5m;
};

extern TStatistics *Statistics;
