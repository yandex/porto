#include <sstream>

#include "statistics.hpp"
#include "data.hpp"
#include "container.hpp"
#include "container_value.hpp"
#include "property.hpp"
#include "network.hpp"
#include "cgroup.hpp"
#include "util/string.hpp"
#include "config.hpp"

extern "C" {
#include <unistd.h>
#include <sys/sysinfo.h>
}

class TIoReadData : public TMapValue, public TContainerValue {
public:
    TIoReadData() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_IO_READ,
                        "read from disk [bytes] (ro)") {}

    TUintMap GetDefault() const override {
        auto memCg = GetContainer()->GetCgroup(MemorySubsystem);
        auto blkCg = GetContainer()->GetCgroup(BlkioSubsystem);
        TUintMap memStat, result;
        TError error;

        error = MemorySubsystem.Statistics(memCg, memStat);
        if (!error)
            result["fs"] = memStat["fs_io_bytes"] - memStat["fs_io_write_bytes"];

        std::vector<BlkioStat> blkStat;
        error = BlkioSubsystem.Statistics(blkCg, "blkio.io_service_bytes_recursive", blkStat);
        if (!error) {
            for (auto &s : blkStat)
                result[s.Device] = s.Read;
        }

        return result;
    }
};

class TIoWriteData : public TMapValue, public TContainerValue {
public:
    TIoWriteData() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_IO_WRITE, "written to disk [bytes] (ro)") {}

    TUintMap GetDefault() const override {
        auto memCg = GetContainer()->GetCgroup(MemorySubsystem);
        auto blkCg = GetContainer()->GetCgroup(BlkioSubsystem);
        TUintMap memStat, result;
        TError error;

        error = MemorySubsystem.Statistics(memCg, memStat);
        if (!error)
            result["fs"] = memStat["fs_io_write_bytes"];

        std::vector<BlkioStat> blkStat;
        error = BlkioSubsystem.Statistics(blkCg, "blkio.io_service_bytes_recursive", blkStat);
        if (!error) {
            for (auto &s : blkStat)
                result[s.Device] = s.Write;
        }

        return result;
    }
};

class TIoOpsData : public TMapValue, public TContainerValue {
public:
    TIoOpsData() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_IO_OPS, "io operations (ro)") {}

    TUintMap GetDefault() const override {
        auto memCg = GetContainer()->GetCgroup(MemorySubsystem);
        auto blkCg = GetContainer()->GetCgroup(BlkioSubsystem);
        TUintMap result, memStat;
        TError error;

        error = MemorySubsystem.Statistics(memCg, memStat);
        if (!error)
            result["fs"] = memStat["fs_io_operations"];

        std::vector<BlkioStat> blkStat;
        error = BlkioSubsystem.Statistics(blkCg, "blkio.io_serviced_recursive", blkStat);
        if (!error) {
            for (auto &s : blkStat)
                result[s.Device] = s.Read + s.Write;
        }

        return result;
    }
};

class TTimeData : public TUintValue, public TContainerValue {
public:
    TTimeData() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_TIME, "running time [seconds] (ro)") {}

    uint64_t GetDefault() const override {
        auto c = GetContainer();

        if (c->IsRoot()) {
            struct sysinfo si;
            int ret = sysinfo(&si);
            if (ret)
                return -1;

            return si.uptime;
        }

        // we started recording raw start/death time since porto v1.15;
        // in case we updated from old version, return zero
        if (!c->StartTime) {
            c->StartTime = GetCurrentTimeMs();
            c->PropMask |= START_TIME_SET;
        }

        if (!c->DeathTime) {
            c->DeathTime = GetCurrentTimeMs();
            c->PropMask |= DEATH_TIME_SET;
        }

        if (c->GetState() == EContainerState::Dead)
            return (c->DeathTime - c->StartTime) / 1000;
        else
            return (GetCurrentTimeMs() - c->StartTime) / 1000;
    }
};

class TPortoStatData : public TMapValue, public TContainerValue {
public:
    TPortoStatData() :
        TMapValue(READ_ONLY_VALUE | HIDDEN_VALUE),
        TContainerValue(D_PORTO_STAT,
                        "porto statistics (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;

        m["spawned"] = Statistics->Spawned;
        m["errors"] = Statistics->Errors;
        m["warnings"] = Statistics->Warns;
        m["master_uptime"] = (GetCurrentTimeMs() - Statistics->MasterStarted) / 1000;
        m["slave_uptime"] = (GetCurrentTimeMs() - Statistics->SlaveStarted) / 1000;
        m["queued_statuses"] = Statistics->QueuedStatuses;
        m["queued_events"] = Statistics->QueuedEvents;
        m["created"] = Statistics->Created;
        m["remove_dead"] = Statistics->RemoveDead;
        m["slave_timeout_ms"] = Statistics->SlaveTimeoutMs;
        m["rotated"] = Statistics->Rotated;
        m["restore_failed"] = Statistics->RestoreFailed;
        m["started"] = Statistics->Started;
        m["running"] = GetContainer()->GetRunningChildren();
        uint64_t usage = 0;
        auto cg = MemorySubsystem.Cgroup(PORTO_DAEMON_CGROUP);
        TError error = MemorySubsystem.Usage(cg, usage);
        if (error)
            L_ERR() << "Can't get memory usage of portod" << std::endl;
        m["memory_usage_mb"] = usage / 1024 / 1024;
        m["epoll_sources"] = Statistics->EpollSources;
        m["containers"] = Statistics->Containers;
        m["volumes"] = Statistics->Volumes;
        m["clients"] = Statistics->Clients;

        return m;
    }
};

void RegisterData(std::shared_ptr<TRawValueMap> m,
                  std::shared_ptr<TContainer> c) {
    const std::vector<TValue *> data = {
        new TIoReadData,
        new TIoWriteData,
        new TIoOpsData,
        new TTimeData,
        new TPortoStatData,
    };

    for (auto d : data)
        AddContainerValue(m, c, d);
}
