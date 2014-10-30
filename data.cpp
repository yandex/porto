#include <sstream>

#include "data.hpp"
#include "container.hpp"
#include "property.hpp"
#include "subsystem.hpp"
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

class TStateData : public TValueDef {
public:
    TStateData() : TValueDef("state",
                             EValueType::String,
                             "container state",
                             NODEF_VALUE,
                             anyState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        return ContainerStateName(c->GetState());    }
};

class TOomKilledData : public TValueDef {
public:
    TOomKilledData() : TValueDef("oom_killed",
                                 EValueType::Bool,
                                 "indicates whether container has been killed by OOM",
                                 NODEF_VALUE,
                                 dState) {}

    bool GetBool(std::shared_ptr<TContainer> c,
                 std::shared_ptr<TValueState> s) {
        return c->IsOomKilled();
    }
};

class TParentData : public TValueDef {
public:
    TParentData() : TValueDef("parent",
                              EValueType::String,
                              "container parent",
                              NODEF_VALUE,
                              anyState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        return c->GetParent() ? c->GetParent()->GetName() : "";
    }
};

class TRespawnCountData : public TValueDef {
public:
    TRespawnCountData() : TValueDef("respawn_count",
                                    // TODO: int
                                    EValueType::String,
                                    "how many time container was automatically respawned",
                                    NODEF_VALUE,
                                    rdState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        return std::to_string(c->GetRespawnCount());
    }
};

class TRootPidData : public TValueDef {
public:
    TRootPidData() : TValueDef("root_pid",
                               // TODO: int
                               EValueType::String,
                               "root process id",
                               NODEF_VALUE,
                               rpState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        if (c->Task)
            return std::to_string(c->Task->GetPid());
        else
            return "-1";
    }
};

class TExitStatusData : public TValueDef {
public:
    TExitStatusData() : TValueDef("exit_status",
                                  // TODO: int
                                  EValueType::String,
                                  "container exit status",
                                  NODEF_VALUE,
                                  dState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        if (c->Task && !c->Task->IsRunning())
            return std::to_string(c->Task->GetExitStatus());
        else
            return "-1";
    }
};

class TStartErrnoData : public TValueDef {
public:
    TStartErrnoData() : TValueDef("start_errno",
                                  // TODO: int
                                  EValueType::String,
                                  "container start error",
                                  NODEF_VALUE,
                                  sState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        return std::to_string(c->GetTaskStartErrno());
    }
};

class TStdoutData : public TValueDef {
public:
    TStdoutData() : TValueDef("stdout",
                              EValueType::String,
                              "return task stdout",
                              NODEF_VALUE,
                              rpdState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        if (c->Task)
            return c->Task->GetStdout(c->Prop->GetUint("stdout_limit"));
        return "";
    }
};

class TStderrData : public TValueDef {
public:
    TStderrData() : TValueDef("stderr",
                              EValueType::String,
                              "return task stderr",
                              NODEF_VALUE,
                              rpdState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        if (c->Task)
            return c->Task->GetStderr(c->Prop->GetUint("stdout_limit"));
        return "";
    }
};

class TCpuUsageData : public TValueDef {
public:
    TCpuUsageData() : TValueDef("cpu_usage",
                                // TODO: int
                                EValueType::String,
                                "return consumed CPU time in nanoseconds",
                                NODEF_VALUE,
                                rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        auto subsys = cpuacctSubsystem;
        auto cg = c->GetLeafCgroup(subsys);
        if (!cg) {
            TLogger::LogAction("cpuacct cgroup not found");
            return "-1";
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
        if (error) {
            TLogger::LogError(error, "Can't get CPU usage");
            return "-1";
        }

        return std::to_string(val);
    }
};

class TMemUsageData : public TValueDef {
public:
    TMemUsageData() : TValueDef("memory_usage",
                                // TODO: int
                                EValueType::String,
                                "return consumed memory in bytes",
                                NODEF_VALUE,
                                rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        auto subsys = memorySubsystem;
        auto cg = c->GetLeafCgroup(subsys);
        if (!cg) {
            TLogger::LogAction("memory cgroup not found");
            return "-1";
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
        if (error) {
            TLogger::LogError(error, "Can't get memory usage");
            return "-1";
        }

        return std::to_string(val);
    }
};

class TNetBytesData : public TValueDef {
public:
    TNetBytesData() : TValueDef("net_bytes",
                                // TODO: map
                                EValueType::String,
                                "number of tx bytes",
                                NODEF_VALUE,
                                rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        uint64_t val;
        TError error = c->GetStat(ETclassStat::Bytes, val);
        if (error) {
            TLogger::LogError(error, "Can't get transmitted bytes");
            return "-1";
        }

        return std::to_string(val);
    }
};

class TNetPacketsData : public TValueDef {
public:
    TNetPacketsData() : TValueDef("net_packets",
                                  // TODO: map
                                  EValueType::String,
                                  "number of tx packets",
                                  NODEF_VALUE,
                                  rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        uint64_t val;
        TError error = c->GetStat(ETclassStat::Packets, val);
        if (error) {
            TLogger::LogError(error, "Can't get transmitted packets");
            return "-1";
        }

        return std::to_string(val);
    }
};

class TNetDropsData : public TValueDef {
public:
    TNetDropsData() : TValueDef("net_drops",
                                // TODO: map
                                EValueType::String,
                                "number of dropped tx packets",
                                NODEF_VALUE,
                                rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        uint64_t val;
        TError error = c->GetStat(ETclassStat::Drops, val);
        if (error) {
            TLogger::LogError(error, "Can't get dropped packets");
            return "-1";
        }

        return std::to_string(val);
    }
};

class TNetOverlimitsData : public TValueDef {
public:
    TNetOverlimitsData() : TValueDef("net_overlimits",
                                     // TODO: map
                                     EValueType::String,
                                     "number of tx packets that exceeded the limit",
                                     NODEF_VALUE,
                                     rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        uint64_t val;
        TError error = c->GetStat(ETclassStat::Overlimits, val);
        if (error) {
            TLogger::LogError(error, "Can't get number of packets over limit");
            return "-1";
        }

        return std::to_string(val);
    }
};

class TMinorFaultsData : public TValueDef {
public:
    TMinorFaultsData() : TValueDef("minor_faults",
                                   // TODO: int
                                   EValueType::String,
                                   "return number of minor page faults",
                                   NODEF_VALUE,
                                   rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        uint64_t val;
        auto cg = c->GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "total_pgfault", val);
        if (error)
            return "-1";

        return std::to_string(val);
    }
};

class TMajorFaultsData : public TValueDef {
public:
    TMajorFaultsData() : TValueDef("major_faults",
                                   // TODO: int
                                   EValueType::String,
                                   "return number of major page faults",
                                   NODEF_VALUE,
                                   rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        uint64_t val;
        auto cg = c->GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "total_pgmajfault", val);
        if (error)
            return "-1";

        return std::to_string(val);
    }
};

class TIoReadData : public TValueDef {
public:
    TIoReadData() : TValueDef("io_read",
                              // TODO: map
                              EValueType::String,
                              "return number of bytes read from disk",
                              NODEF_VALUE | HIDDEN_DATA,
                              rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        auto cg = c->GetLeafCgroup(blkioSubsystem);

        std::vector<BlkioStat> stat;
        TError error = blkioSubsystem->Statistics(cg, "blkio.io_service_bytes_recursive", stat);
        if (error)
            return "-1";

        std::stringstream str;
        for (auto &s : stat) {
            if (str.str().length())
                str << " ";
            str << s.Device << ":" << s.Read;
        }

        return str.str();
    }
};

class TIoWriteData : public TValueDef {
public:
    TIoWriteData() : TValueDef("io_write",
                               // TODO: map
                               EValueType::String,
                               "return number of bytes written to disk",
                               NODEF_VALUE | HIDDEN_DATA,
                               rpdmState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        auto cg = c->GetLeafCgroup(blkioSubsystem);

        std::vector<BlkioStat> stat;
        TError error = blkioSubsystem->Statistics(cg, "blkio.io_service_bytes_recursive", stat);
        if (error)
            return "-1";

        std::stringstream str;
        for (auto &s : stat) {
            if (str.str().length())
                str << " ";
            str << s.Device << ":" << s.Write;
        }

        return str.str();
    }
};

class TTimeData : public TValueDef {
public:
    TTimeData() : TValueDef("minor_faults",
                            // TODO: int
                            EValueType::String,
                            "return running time of container",
                            NODEF_VALUE,
                            rpdState) {}

    std::string GetString(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
        if (!c->Task || !c->Task->IsRunning())
            return "0";

        int pid = c->Task->GetPid();
        TFile f("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (f.AsString(line))
            return "0";

        std::vector<std::string> cols;
        if (SplitString(line, ' ', cols))
            return "0";

        if (cols.size() <= 21)
            return "0";

        int64_t started;
        if (StringToInt64(cols[21], started))
            return "0";

        started /= sysconf(_SC_CLK_TCK);
        started += BootTime;

        return std::to_string(time(nullptr) - started);
    }
};

TValueSpec dataSpec;
TError RegisterData() {
    std::vector<TValueDef *> dat = {
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
    };

    return dataSpec.Register(dat);
}
