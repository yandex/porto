#include <sstream>

#include "statistics.hpp"
#include "data.hpp"
#include "container.hpp"
#include "container_value.hpp"
#include "property.hpp"
#include "network.hpp"
#include "cgroup.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "config.hpp"

extern "C" {
#include <unistd.h>
#include <sys/sysinfo.h>
}

static std::set<EContainerState> anyState = {
    EContainerState::Stopped,
    EContainerState::Dead,
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Meta
};

static std::set<EContainerState> dState = {
    EContainerState::Dead,
};

static std::set<EContainerState> rdState = {
    EContainerState::Running,
    EContainerState::Dead,
};

static std::set<EContainerState> rpState = {
    EContainerState::Running,
    EContainerState::Paused,
};

static std::set<EContainerState> rpdState = {
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Dead,
};

static std::set<EContainerState> rpmState = {
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Meta,
};

static std::set<EContainerState> rpdmState = {
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Dead,
    EContainerState::Meta,
};

static std::set<EContainerState> sState = {
    EContainerState::Stopped,
};

class TStateData : public TStringValue, public TContainerValue {
public:
    TStateData() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(D_STATE,
                        "container state",
                        anyState) {}
};

class TOomKilledData : public TBoolValue, public TContainerValue {
public:
    TOomKilledData() :
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(D_OOM_KILLED,
                        "indicates whether container has been killed by OOM",
                        dState) {}
};

class TAbsoluteNameData : public TStringValue, public TContainerValue {
public:
    TAbsoluteNameData() :
        TStringValue(0),
        TContainerValue(D_ABSOLUTE_NAME,
                        "Absolute name of Porto container",
                        anyState) {};

    std::string GetDefault() const override {
        return GetContainer()->GetName();
    }
};

class TParentData : public TStringValue, public TContainerValue {
public:
    TParentData() :
        TStringValue(HIDDEN_VALUE),
        TContainerValue(D_PARENT,
                        "parent container name (deprecated)",
                        anyState) {}

    std::string GetDefault() const override {
        return GetContainer()->GetParent() ?
            GetContainer()->GetParent()->GetName() : "";
    }
};

class TRespawnCountData : public TUintValue, public TContainerValue {
public:
    TRespawnCountData() :
        TUintValue(PERSISTENT_VALUE),
        TContainerValue(D_RESPAWN_COUNT,
                        "how many times container was automatically respawned",
                        rdState) {}
};

class TRootPidData : public TIntValue, public TContainerValue {
public:
    TRootPidData() :
        TIntValue(HIDDEN_VALUE),
        TContainerValue(D_ROOT_PID,
                        "root process id (deprecated)",
                        rpmState) {}

    int GetDefault() const override {
        auto c = GetContainer();
        std::vector<int> pids;

        if (!c->Prop->HasValue(P_RAW_ROOT_PID))
            return -1;

        pids = c->Prop->Get<std::vector<int>>(P_RAW_ROOT_PID);
        return pids[0];
    }
};

class TExitStatusData : public TIntValue, public TContainerValue {
public:
    TExitStatusData() :
        TIntValue(PERSISTENT_VALUE),
        TContainerValue(D_EXIT_STATUS,
                        "container exit status",
                        dState) {}
};

class TStartErrnoData : public TIntValue, public TContainerValue {
public:
    TStartErrnoData() :
        TIntValue(0),
        TContainerValue(D_START_ERRNO,
                        "container start error",
                        sState) {}
};

class TStdoutData : public TTextValue, public TContainerValue {
public:
    TStdoutData() :
        TTextValue(READ_ONLY_VALUE),
        TContainerValue(D_STDOUT,
                        "return task stdout",
                        rpdState) {}

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
        TTextValue(READ_ONLY_VALUE),
        TContainerValue(D_STDERR,
                        "return task stderr",
                        rpdState) {}


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
        TUintValue(READ_ONLY_VALUE),
        TContainerValue(D_STDOUT_OFFSET, "stdout offset", rpdState) {}
};

class TStderrOffset : public TUintValue, public TContainerValue {
public:
    TStderrOffset() :
        TUintValue(READ_ONLY_VALUE),
        TContainerValue(D_STDERR_OFFSET, "stderr offset", rpdState) {}
};

class TCpuUsageData : public TUintValue, public TContainerValue {
public:
    TCpuUsageData() :
        TUintValue(0),
        TContainerValue(D_CPU_USAGE,
                        "return consumed CPU time in nanoseconds",
                        rpdmState) {}

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
        TUintValue(0),
        TContainerValue(D_MEMORY_USAGE,
                        "return consumed memory in bytes",
                        rpdmState) {}

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

class TNetBytesData : public TMapValue, public TContainerValue {
public:
    TNetBytesData() :
        TMapValue(0),
        TContainerValue(D_NET_BYTES,
                        "number of tx bytes",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Bytes, m);
        return m;
    }
};

class TNetPacketsData : public TMapValue, public TContainerValue {
public:
    TNetPacketsData() :
        TMapValue(0),
        TContainerValue(D_NET_PACKETS,
                        "number of tx packets",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Packets, m);
        return m;
    }
};

class TNetDropsData : public TMapValue, public TContainerValue {
public:
    TNetDropsData() :
        TMapValue(0),
        TContainerValue(D_NET_DROPS,
                        "number of dropped tx packets",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Drops, m);
        return m;
    }
};

class TNetOverlimitsData : public TMapValue, public TContainerValue {
public:
    TNetOverlimitsData() :
        TMapValue(0),
        TContainerValue(D_NET_OVERLIMITS,
                        "number of tx packets that exceeded the limit",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::Overlimits, m);
        return m;
    }
};

class TNetRxBytes : public TMapValue, public TContainerValue {
public:
    TNetRxBytes() :
        TMapValue(0),
        TContainerValue(D_NET_RX_BYTES,
                        "number of rx bytes",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::RxBytes, m);
        return m;
    }
};

class TNetRxPackets : public TMapValue, public TContainerValue {
public:
    TNetRxPackets() :
        TMapValue(0),
        TContainerValue(D_NET_RX_PACKETS,
                        "number of rx packets",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::RxPackets, m);
        return m;
    }
};

class TNetRxDrops : public TMapValue, public TContainerValue {
public:
    TNetRxDrops() :
        TMapValue(0),
        TContainerValue(D_NET_RX_DROPS,
                        "number of dropped rx packets",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        (void)GetContainer()->GetStat(ETclassStat::RxDrops, m);
        return m;
    }
};

class TMinorFaultsData : public TUintValue, public TContainerValue {
public:
    TMinorFaultsData() :
        TUintValue(0),
        TContainerValue(D_MINOR_FAULTS,
                        "return number of minor page faults",
                        rpdmState) {}

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
        TUintValue(0),
        TContainerValue(D_MAJOR_FAULTS,
                        "return number of major page faults",
                        rpdmState) {}

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
        TMapValue(0),
        TContainerValue(D_IO_READ,
                        "return number of bytes read from disk",
                        rpdmState) {}

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
        TMapValue(0),
        TContainerValue(D_IO_WRITE,
                        "return number of bytes written to disk",
                        rpdmState) {}

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
        TMapValue(0),
        TContainerValue(D_IO_OPS,
                        "return number of disk io operations",
                        rpdmState) {}

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
        TUintValue(0),
        TContainerValue(D_TIME,
                        "container running time",
                        rpdmState) {}

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
        TUintValue(0),
        TContainerValue(D_MAX_RSS,
                        "maximum amount of anonymous memory container consumed",
                        rpdmState) {
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
        TMapValue(HIDDEN_VALUE),
        TContainerValue(D_PORTO_STAT,
                        "",
                        anyState) {}

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
