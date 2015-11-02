#include <sstream>

#include "statistics.hpp"
#include "data.hpp"
#include "container.hpp"
#include "container_value.hpp"
#include "property.hpp"
#include "subsystem.hpp"
#include "qdisc.hpp"
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

static std::string ReadStdio(const TPath &path, size_t limit) {
    if (!path.IsRegular())
        return "";

    std::string s;
    TFile f(path);
    TError error(f.LastStrings(limit, s));
    if (error)
        L_ERR() << "Can't read container stdout: " << error << std::endl;
    return s;
}

class TStdoutData : public TStringValue, public TContainerValue {
public:
    TStdoutData() :
        TStringValue(0),
        TContainerValue(D_STDOUT,
                        "return task stdout",
                        rpdState) {}

    std::string GetDefault() const override {
        auto c = GetContainer();
        return ReadStdio(c->Prop->Get<std::string>(P_STDOUT_PATH),
                         c->Prop->Get<uint64_t>(P_STDOUT_LIMIT));
    }
};

class TStderrData : public TStringValue, public TContainerValue {
public:
    TStderrData() :
        TStringValue(0),
        TContainerValue(D_STDERR,
                        "return task stderr",
                        rpdState) {}

    std::string GetDefault() const override {
        auto c = GetContainer();
        return ReadStdio(c->Prop->Get<std::string>(P_STDERR_PATH),
                         c->Prop->Get<uint64_t>(P_STDOUT_LIMIT));
    }
};

class TCpuUsageData : public TUintValue, public TContainerValue {
public:
    TCpuUsageData() :
        TUintValue(0),
        TContainerValue(D_CPU_USAGE,
                        "return consumed CPU time in nanoseconds",
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
        TUintValue(0),
        TContainerValue(D_MEMORY_USAGE,
                        "return consumed memory in bytes",
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

class TMinorFaultsData : public TUintValue, public TContainerValue {
public:
    TMinorFaultsData() :
        TUintValue(0),
        TContainerValue(D_MINOR_FAULTS,
                        "return number of minor page faults",
                        rpdmState) {}

    uint64_t GetDefault() const override {
        uint64_t minor_faults = 0;
        auto cg = GetContainer()->GetLeafCgroup(memorySubsystem);
        int left = 2;
        TError error = memorySubsystem->Statistics(cg, [&](std::string k, uint64_t v) -> int {
            if (k == "total_pgfault") {
                minor_faults += v;
                left--;
            } else if (k == "total_pgmajfault") {
                minor_faults -= v;
                left--;
            }
            return left;
        });
        if (error)
            return -1;
        return minor_faults;
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
        TMapValue(0),
        TContainerValue(D_IO_READ,
                        "return number of bytes read from disk",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TError error;
        TUintMap m;

        auto memcg = GetContainer()->GetLeafCgroup(memorySubsystem);

        uint64_t read_bytes = 0;
        int left = 2;
        error = memorySubsystem->Statistics(memcg, [&](std::string k, uint64_t v) -> int {
            if (k == "fs_io_bytes") {
                read_bytes += v;
                left--;
            } else if (k == "fs_io_write_bytes") {
                read_bytes -= v;
                left--;
            }
            return left;
        });
        if (!error)
            m["fs"] = read_bytes;

        auto blkcg = GetContainer()->GetLeafCgroup(blkioSubsystem);

        std::vector<BlkioStat> stat;
        error = blkioSubsystem->Statistics(blkcg, "blkio.io_service_bytes_recursive", stat);
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
        TMapValue(0),
        TContainerValue(D_IO_WRITE,
                        "return number of bytes written to disk",
                        rpdmState) {}

    TUintMap GetDefault() const override {
        TError error;
        TUintMap m;

        auto memcg = GetContainer()->GetLeafCgroup(memorySubsystem);

        uint64_t write_bytes;
        error = memorySubsystem->Statistics(memcg, "fs_io_write_bytes", write_bytes);
        if (!error)
            m["fs"] = write_bytes;

        auto blkcg = GetContainer()->GetLeafCgroup(blkioSubsystem);

        std::vector<BlkioStat> stat;
        error = blkioSubsystem->Statistics(blkcg, "blkio.io_service_bytes_recursive", stat);
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
        uint64_t val;
        auto err = memorySubsystem->Statistics(memorySubsystem->GetRootCgroup(),
                                               "total_max_rss", val);
        if (err)
            Implemented = false;
    }

    uint64_t GetDefault() const override {
        uint64_t val;
        auto cg = GetContainer()->GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "total_max_rss", val);
        if (error)
            return 0;

        return val;
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
        TError error = memorySubsystem->Usage(memorySubsystem->GetRootCgroup()->GetChild(PORTO_DAEMON_CGROUP), usage);
        if (error)
            L_ERR() << "Can't get memory usage of portod" << std::endl;
        m["memory_usage_mb"] = usage / 1024 / 1024;
        m["epoll_sources"] = Statistics->EpollSources;

        return m;
    }
};

void RegisterData(std::shared_ptr<TRawValueMap> m,
                  std::shared_ptr<TContainer> c) {
    const std::vector<TAbstractValue *> data = {
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
