#include <sstream>

#include "data.hpp"
#include "container.hpp"
#include "property.hpp"
#include "subsystem.hpp"
#include "qdisc.hpp"
#include "util/file.hpp"
#include "util/string.hpp"

extern "C" {
#include <unistd.h>
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
        TContainerValue(D_STATE,
                        "container state",
                        NODEF_VALUE | PERSISTENT_VALUE,
                        anyState) {}
};

class TOomKilledData : public TBoolValue, public TContainerValue {
public:
    TOomKilledData() :
        TContainerValue(D_OOM_KILLED,
                        "indicates whether container has been killed by OOM",
                        NODEF_VALUE | PERSISTENT_VALUE,
                        dState) {}
};

class TParentData : public TStringValue, public TContainerValue {
public:
    TParentData() :
        TContainerValue(D_PARENT,
                        "parent container",
                        NODEF_VALUE,
                        anyState) {}

    std::string GetDefault() const override {
        return GetContainer()->GetParent() ?
            GetContainer()->GetParent()->GetName() : "";
    }
};

class TRespawnCountData : public TUintValue, public TContainerValue {
public:
    TRespawnCountData() :
        TContainerValue(D_RESPAWN_COUNT,
                        "how many times container was automatically respawned",
                        NODEF_VALUE | PERSISTENT_VALUE,
                        rdState) {}
};

class TRootPidData : public TIntValue, public TContainerValue {
public:
    TRootPidData() :
        TContainerValue(D_ROOT_PID,
                        "root process id",
                        NODEF_VALUE,
                        rpState) {}

    int GetDefault() const override {
        if (!GetContainer()->Task)
            return -1;
        return GetContainer()->Task->GetPid();
    }
};

class TExitStatusData : public TIntValue, public TContainerValue {
public:
    TExitStatusData() :
        TContainerValue(D_EXIT_STATUS,
                        "container exit status",
                        NODEF_VALUE | PERSISTENT_VALUE,
                        dState) {}
};

class TStartErrnoData : public TIntValue, public TContainerValue {
public:
    TStartErrnoData() :
        TContainerValue(D_START_ERRNO,
                        "container start error",
                        NODEF_VALUE,
                        sState) {}
};

class TStdoutData : public TStringValue, public TContainerValue {
public:
    TStdoutData() :
        TContainerValue(D_STDOUT,
                        "return task stdout",
                        NODEF_VALUE,
                        rpdState) {}

    std::string GetDefault() const override {
        auto c = GetContainer();
        if (c->Task)
            return c->Task->GetStdout(c->Prop->GetUint(P_STDOUT_LIMIT));
        return "";
    }
};

class TStderrData : public TStringValue, public TContainerValue {
public:
    TStderrData() :
        TContainerValue(D_STDERR,
                        "return task stderr",
                        NODEF_VALUE,
                        rpdState) {}

    std::string GetDefault() const override {
        auto c = GetContainer();
        if (c->Task)
            return c->Task->GetStderr(c->Prop->GetUint(P_STDOUT_LIMIT));
        return "";
    }
};

class TCpuUsageData : public TUintValue, public TContainerValue {
public:
    TCpuUsageData() :
        TContainerValue(D_CPU_USAGE,
                        "return consumed CPU time in nanoseconds",
                        NODEF_VALUE,
                        rpdmState) {}

    uint64_t GetDefault() const override {
        auto subsys = cpuacctSubsystem;
        auto cg = GetContainer()->GetLeafCgroup(subsys);
        if (!cg) {
            L_ERR() << "Can't find cpuacct cgroup" << std::endl;
            return -1;
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
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
        TContainerValue(D_MEMORY_USAGE,
                        "return consumed memory in bytes",
                        NODEF_VALUE,
                        rpdmState) {}

    uint64_t GetDefault() const override {
        auto subsys = memorySubsystem;
        auto cg = GetContainer()->GetLeafCgroup(subsys);
        if (!cg) {
            L_ERR() << "Can't find memory cgroup" << std::endl;
            return -1;
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
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
        TContainerValue(D_NET_BYTES,
                        "number of tx bytes",
                        NODEF_VALUE,
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        TError error = GetContainer()->GetStat(ETclassStat::Bytes, m);
        if (error)
            L_ERR() << "Can't get transmitted bytes: " << error << std::endl;
        return m;
    }
};

class TNetPacketsData : public TMapValue, public TContainerValue {
public:
    TNetPacketsData() :
        TContainerValue(D_NET_PACKETS,
                        "number of tx packets",
                        NODEF_VALUE,
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        TError error = GetContainer()->GetStat(ETclassStat::Packets, m);
        if (error)
            L_ERR() << "Can't get transmitted packets: " << error << std::endl;
        return m;
    }
};

class TNetDropsData : public TMapValue, public TContainerValue {
public:
    TNetDropsData() :
        TContainerValue(D_NET_DROPS,
                        "number of dropped tx packets",
                        NODEF_VALUE,
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        TError error = GetContainer()->GetStat(ETclassStat::Drops, m);
        if (error)
            L_ERR() << "Can't get dropped packets: " << error << std::endl;
        return m;
    }
};

class TNetOverlimitsData : public TMapValue, public TContainerValue {
public:
    TNetOverlimitsData() :
        TContainerValue(D_NET_OVERLIMITS,
                        "number of tx packets that exceeded the limit",
                        NODEF_VALUE,
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        TError error = GetContainer()->GetStat(ETclassStat::Overlimits, m);
        if (error)
            L_ERR() << "Can't get number of packets over limit: " << error << std::endl;
        return m;
    }
};

class TMinorFaultsData : public TUintValue, public TContainerValue {
public:
    TMinorFaultsData() :
        TContainerValue(D_MINOR_FAULTS,
                        "return number of minor page faults",
                        NODEF_VALUE,
                        rpdmState) {}

    uint64_t GetDefault() const override {
        uint64_t val;
        auto cg = GetContainer()->GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "total_pgfault", val);
        if (error)
            return -1;

        return val;
    }
};

class TMajorFaultsData : public TUintValue, public TContainerValue {
public:
    TMajorFaultsData() :
        TContainerValue(D_MAJOR_FAULTS,
                        "return number of major page faults",
                        NODEF_VALUE,
                        rpdmState) {}

    uint64_t GetDefault() const override {
        uint64_t val;
        auto cg = GetContainer()->GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "total_pgmajfault", val);
        if (error)
            return -1;

        return val;
    }
};

class TIoReadData : public TMapValue, public TContainerValue {
public:
    TIoReadData() :
        TContainerValue(D_IO_READ,
                        "return number of bytes read from disk",
                        NODEF_VALUE,
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        auto cg = GetContainer()->GetLeafCgroup(blkioSubsystem);

        std::vector<BlkioStat> stat;
        TError error = blkioSubsystem->Statistics(cg, "blkio.io_service_bytes_recursive", stat);
        if (error)
            L_ERR() << "Can't get blkio statistics: " << error << std::endl;
        if (error)
            return m;

        for (auto &s : stat)
            m[s.Device] = s.Read;

        return m;
    }
};

class TIoWriteData : public TMapValue, public TContainerValue {
public:
    TIoWriteData() :
        TContainerValue(D_IO_WRITE,
                        "return number of bytes written to disk",
                        NODEF_VALUE,
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TUintMap m;
        auto cg = GetContainer()->GetLeafCgroup(blkioSubsystem);

        std::vector<BlkioStat> stat;
        TError error = blkioSubsystem->Statistics(cg, "blkio.io_service_bytes_recursive", stat);
        if (error)
            L_ERR() << "Can't get blkio statistics: " << error << std::endl;
        if (error)
            return m;

        for (auto &s : stat)
            m[s.Device] = s.Write;

        return m;
    }
};

class TTimeData : public TUintValue, public TContainerValue {
public:
    TTimeData() :
        TContainerValue(D_TIME,
                        "root process running time",
                        NODEF_VALUE,
                        rpdState) {}

    uint64_t GetDefault() const override {
        auto c = GetContainer();
        if (c->GetState() == EContainerState::Dead)
            return (GetCurrentTimeMs() - c->GetTimeOfDeath()) / 1000;

        if (!c->Task || !c->Task->IsRunning())
            return 0;

        int pid = c->Task->GetPid();
        TFile f("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (f.AsString(line))
            return 0;

        std::vector<std::string> cols;
        if (SplitString(line, ' ', cols))
            return 0;

        if (cols.size() <= 21)
            return 0;

        int64_t started;
        if (StringToInt64(cols[21], started))
            return 0;

        started /= sysconf(_SC_CLK_TCK);
        started += BootTime;

        return time(nullptr) - started;
    }
};

class TMaxRssData : public TUintValue, public TContainerValue {
public:
    TMaxRssData() :
        TContainerValue(D_MAX_RSS,
                        "Guaranteed amount of memory",
                        NODEF_VALUE,
                        rpdmState) {}

    uint64_t GetDefault() const override {
        uint64_t val;
        auto cg = GetContainer()->GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "max_rss", val);
        if (error)
            return 0;

        return val;
    }
};

class TPortoStatData : public TMapValue, public TContainerValue {
public:
    TPortoStatData() :
        TContainerValue(D_PORTO_STAT,
                        "",
                        NODEF_VALUE | HIDDEN_VALUE,
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

        return m;
    }
};

void RegisterData(std::shared_ptr<TRawValueMap> m,
                  std::shared_ptr<TContainer> c) {
    std::vector<TAbstractValue *> data = {
        new TStateData,
        new TOomKilledData,
        new TParentData,
        new TRespawnCountData,
        new TRootPidData,
        new TExitStatusData,
        new TStartErrnoData,
        new TStdoutData,
        new TStderrData,
        new TCpuUsageData,
        new TMemUsageData,
        new TNetBytesData,
        new TNetPacketsData,
        new TNetDropsData,
        new TNetOverlimitsData,
        new TMinorFaultsData,
        new TMajorFaultsData,
        new TIoReadData,
        new TIoWriteData,
        new TTimeData,
        new TMaxRssData,
        new TPortoStatData,
    };

    for (auto d : data)
        AddContainerValue(m, c, d);
}
