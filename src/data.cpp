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

class TStateData : public TStringValue, public TContainerValue {
public:
    TStateData() :
        TStringValue(READ_ONLY_VALUE | PERSISTENT_VALUE),
        TContainerValue(D_STATE,
                        "container state (ro)") {}
};

class TOomKilledData : public TBoolValue, public TContainerValue {
public:
    TOomKilledData() :
        TBoolValue(READ_ONLY_VALUE | PERSISTENT_VALUE | POSTMORTEM_VALUE),
        TContainerValue(D_OOM_KILLED, "container has been killed by OOM (ro)") {}
};

class TAbsoluteNameData : public TStringValue, public TContainerValue {
public:
    TAbsoluteNameData() :
        TStringValue(READ_ONLY_VALUE),
        TContainerValue(D_ABSOLUTE_NAME,
                        "container name including porto namespaces (ro)") {};

    std::string GetDefault() const override {
        auto ct = GetContainer();
        if (ct->IsRoot() || ct->IsPortoRoot())
            return ct->GetName();
        return std::string(PORTO_ROOT_CONTAINER) + "/" + ct->GetName();
    }
};

class TAbsoluteNamespaceData : public TStringValue, public TContainerValue {
public:
    TAbsoluteNamespaceData() :
        TStringValue(READ_ONLY_VALUE),
        TContainerValue(D_ABSOLUTE_NAMESPACE,
                        "container namespace including parent namespaces (ro)") {};

    std::string GetDefault() const override {
        return std::string(PORTO_ROOT_CONTAINER) + "/" +
                        GetContainer()->GetPortoNamespace();
    }
};

class TParentData : public TStringValue, public TContainerValue {
public:
    TParentData() :
        TStringValue(READ_ONLY_VALUE | HIDDEN_VALUE),
        TContainerValue(D_PARENT,
                        "parent container name (ro) (deprecated)") {}

    std::string GetDefault() const override {
        return GetContainer()->GetParent() ?
            GetContainer()->GetParent()->GetName() : "";
    }
};

class TRespawnCountData : public TUintValue, public TContainerValue {
public:
    TRespawnCountData() :
        TUintValue(READ_ONLY_VALUE | PERSISTENT_VALUE),
        TContainerValue(D_RESPAWN_COUNT,
                        "current respawn count (ro)") {}
};

class TRootPidData : public TIntValue, public TContainerValue {
public:
    TRootPidData() :
        TIntValue(READ_ONLY_VALUE | HIDDEN_VALUE | RUNTIME_VALUE),
        TContainerValue(D_ROOT_PID, "root task pid (ro)") {}
};

class TExitStatusData : public TIntValue, public TContainerValue {
public:
    TExitStatusData() :
        TIntValue(READ_ONLY_VALUE | PERSISTENT_VALUE | POSTMORTEM_VALUE),
        TContainerValue(D_EXIT_STATUS,
                        "container exit status (ro)") {}
};

class TStartErrnoData : public TIntValue, public TContainerValue {
public:
    TStartErrnoData() :
        TIntValue(READ_ONLY_VALUE),
        TContainerValue(D_START_ERRNO,
                        "container start error (ro)") {}
};

class TStdoutData : public TTextValue, public TContainerValue {
public:
    TStdoutData() :
        TTextValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_STDOUT,
                        "stdout (optional start [offset]) (ro)") {}

    TError GetString(std::string &value) const override {
        auto c = GetContainer();
        return c->GetStdout().Read(value, c->Prop->Get<uint64_t>(P_STDOUT_LIMIT),
                                   c->Data->Get<uint64_t>(P_STDOUT_OFFSET));
    }

    TError GetIndexed(const std::string &index, std::string &value) const override {
        auto c = GetContainer();
        return c->GetStdout().Read(value, c->Prop->Get<uint64_t>(P_STDOUT_LIMIT),
                                   c->Data->Get<uint64_t>(P_STDOUT_OFFSET),
                                   index);
    }
};

class TStderrData : public TTextValue, public TContainerValue {
public:
    TStderrData() :
        TTextValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_STDERR,
                        "stderr (optional start [offset]) (ro)") {}


    TError GetString(std::string &value) const override {
        auto c = GetContainer();
        return c->GetStderr().Read(value, c->Prop->Get<uint64_t>(P_STDOUT_LIMIT),
                                   c->Data->Get<uint64_t>(P_STDOUT_OFFSET));
    }

    TError GetIndexed(const std::string &index, std::string &value) const override {
        auto c = GetContainer();
        return c->GetStderr().Read(value, c->Prop->Get<uint64_t>(P_STDOUT_LIMIT),
                                   c->Data->Get<uint64_t>(P_STDERR_OFFSET), index);
    }
};

class TStdoutOffset : public TUintValue, public TContainerValue {
public:
    TStdoutOffset() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_STDOUT_OFFSET, "stored stdout offset (ro)") {}
};

class TStderrOffset : public TUintValue, public TContainerValue {
public:
    TStderrOffset() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_STDERR_OFFSET, "stored stderr offset (ro)") {}
};

class TCpuUsageData : public TUintValue, public TContainerValue {
public:
    TCpuUsageData() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_CPU_USAGE,
                        "consumed CPU time [nanoseconds] (ro)") {}

    uint64_t GetDefault() const override {
        auto cg = GetContainer()->GetCgroup(CpuacctSubsystem);

        uint64_t val;
        TError error = CpuacctSubsystem.Usage(cg, val);
        if (error) {
            L_ERR() << "Can't get CPU usage: " << error << std::endl;
            return -1;
        }

        return val;
    }
};

class TMemUsageData : public TUintValue, public TContainerValue {
public:
    TMemUsageData() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_MEMORY_USAGE,
                        "current memory usage [bytes] (ro)") {}

    uint64_t GetDefault() const override {
        auto cg = GetContainer()->GetCgroup(MemorySubsystem);

        uint64_t val;
        TError error = MemorySubsystem.Usage(cg, val);
        if (error) {
            L_ERR() << "Can't get memory usage: " << error << std::endl;
            return -1;
        }

        return val;
    }
};

class TAnonUsageData : public TUintValue, public TContainerValue {
public:
    TAnonUsageData() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_ANON_USAGE,
                        "current anonymous memory usage [bytes] (ro)") {}

    uint64_t GetDefault() const override {
        auto cg = GetContainer()->GetCgroup(MemorySubsystem);
        uint64_t val;
        if (MemorySubsystem.GetAnonUsage(cg, val))
            return 0;
        return val;
    }
};

class TNetBytesData : public TMapValue, public TContainerValue {
public:
    TNetBytesData() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_NET_BYTES,
                        "tx bytes: <interface>: <bytes>;... (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Bytes, m);
        return m;
    }
};

class TNetPacketsData : public TMapValue, public TContainerValue {
public:
    TNetPacketsData() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_NET_PACKETS,
                        "tx packets: <interface>: <packets>;... (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Packets, m);
        return m;
    }
};

class TNetDropsData : public TMapValue, public TContainerValue {
public:
    TNetDropsData() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_NET_DROPS,
                        "dropped tx packets: <interface>: <packets>;... (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Drops, m);
        return m;
    }
};

class TNetOverlimitsData : public TMapValue, public TContainerValue {
public:
    TNetOverlimitsData() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_NET_OVERLIMITS,
                        "overlimit tx packets: <interface>: <packets>;... (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Overlimits, m);
        return m;
    }
};

class TNetRxBytes : public TMapValue, public TContainerValue {
public:
    TNetRxBytes() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_NET_RX_BYTES,
                        "rx bytes: <interface>: <bytes>;... (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::RxBytes, m);
        return m;
    }
};

class TNetRxPackets : public TMapValue, public TContainerValue {
public:
    TNetRxPackets() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_NET_RX_PACKETS,
                        "rx packets: <interface>: <packets>;... (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::RxPackets, m);
        return m;
    }
};

class TNetRxDrops : public TMapValue, public TContainerValue {
public:
    TNetRxDrops() :
        TMapValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_NET_RX_DROPS,
                        "dropped rx packets: <interface>: <packets>;... (ro)") {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::RxDrops, m);
        return m;
    }
};

class TMinorFaultsData : public TUintValue, public TContainerValue {
public:
    TMinorFaultsData() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_MINOR_FAULTS,
                        "minor page faults (ro)") {}

    uint64_t GetDefault() const override {
        auto cg = GetContainer()->GetCgroup(MemorySubsystem);
        TUintMap stat;
        TError error = MemorySubsystem.Statistics(cg, stat);
        if (error)
            return -1;
        return stat["total_pgfault"] - stat["total_pgmajfault"];
    }
};

class TMajorFaultsData : public TUintValue, public TContainerValue {
public:
    TMajorFaultsData() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_MAJOR_FAULTS,
                        "major page faults (ro)") {}

    uint64_t GetDefault() const override {
        auto cg = GetContainer()->GetCgroup(MemorySubsystem);
        TUintMap stat;
        TError error = MemorySubsystem.Statistics(cg, stat);
        if (error)
            return -1;
        return stat["total_pgmajfault"];
    }
};

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
        if (!c->Prop->Get<uint64_t>(P_RAW_START_TIME))
            c->Prop->Set<uint64_t>(P_RAW_START_TIME, GetCurrentTimeMs());

        if (!c->Prop->Get<uint64_t>(P_RAW_DEATH_TIME))
            c->Prop->Set<uint64_t>(P_RAW_DEATH_TIME, GetCurrentTimeMs());

        if (c->GetState() == EContainerState::Dead)
            return (c->Prop->Get<uint64_t>(P_RAW_DEATH_TIME) -
                    c->Prop->Get<uint64_t>(P_RAW_START_TIME)) / 1000;
        else
            return (GetCurrentTimeMs() -
                    c->Prop->Get<uint64_t>(P_RAW_START_TIME)) / 1000;
    }
};

class TMaxRssData : public TUintValue, public TContainerValue {
public:
    TMaxRssData() :
        TUintValue(READ_ONLY_VALUE | RUNTIME_VALUE),
        TContainerValue(D_MAX_RSS,
                        "peak anonymous memory usage [bytes] (ro)") {
        TCgroup rootCg = MemorySubsystem.RootCgroup();
        TUintMap stat;
        TError error = MemorySubsystem.Statistics(rootCg, stat);
        if (error || stat.find("total_max_rss") == stat.end())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    uint64_t GetDefault() const override {
        TCgroup cg = GetContainer()->GetCgroup(MemorySubsystem);
        TUintMap stat;
        MemorySubsystem.Statistics(cg, stat);
        return stat["total_max_rss"];
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
        new TStateData,
        new TAbsoluteNameData,
        new TAbsoluteNamespaceData,
        new TOomKilledData,
        new TParentData,
        new TRespawnCountData,
        new TRootPidData,
        new TExitStatusData,
        new TStartErrnoData,
        new TStdoutData,
        new TStderrData,
        new TStdoutOffset,
        new TStderrOffset,
        new TCpuUsageData,
        new TMemUsageData,
        new TAnonUsageData,
        new TNetBytesData,
        new TNetPacketsData,
        new TNetDropsData,
        new TNetOverlimitsData,
        new TNetRxBytes,
        new TNetRxPackets,
        new TNetRxDrops,
        new TMinorFaultsData,
        new TMajorFaultsData,
        new TIoReadData,
        new TIoWriteData,
        new TIoOpsData,
        new TTimeData,
        new TMaxRssData,
        new TPortoStatData,
    };

    for (auto d : data)
        AddContainerValue(m, c, d);
}
