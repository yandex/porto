#include "property.hpp"
#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "container_value.hpp"
#include "network.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"

extern "C" {
#include <linux/capability.h>
}

extern __thread TContainer *CurrentContainer;
extern __thread TClient *CurrentClient;
extern TContainerUser ContainerUser;
extern TContainerGroup ContainerGroup;
extern std::map<std::string, TContainerProperty*> ContainerPropMap;

bool TPropertyMap::ParentDefault(std::shared_ptr<TContainer> &c,
                                 const std::string &property) const {
    TError error = GetSharedContainer(c);
    if (error) {
        L_ERR() << "Can't get default for " << property << ": " << error << std::endl;
        return false;
    }

    return HasFlags(property, PARENT_DEF_PROPERTY) && !GetRaw<bool>(P_ISOLATE);
}

bool TPropertyMap::HasFlags(const std::string &property, int flags) const {
    auto prop = Find(property);
    if (!prop) {
        L_ERR() << TError(EError::Unknown, "Invalid property " + property) << std::endl;
        return false;
    }
    return prop->HasFlag(flags);
}

TError TPropertyMap::PrepareTaskEnv(const std::string &property,
                                    TTaskEnv &taskEnv) {
    auto prop = Find(property);

    // FIXME must die
    if (!prop->HasValue()) {
        std::string value;
        TError error;

        // if the value is default we still need PrepareTaskEnv method
        // to be called, so set value to default and then reset it
        error = prop->GetString(value);
        if (!error)
            error = prop->SetString(value);
        if (error)
            return error;
        prop->Reset();
    }

    return ToContainerValue(prop)->PrepareTaskEnv(taskEnv);
}

TError TPropertyMap::GetSharedContainer(std::shared_ptr<TContainer> &c) const {
    c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    return TError::Success();
}

class TCommandProperty : public TStringValue, public TContainerValue {
public:
    TCommandProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_COMMAND,
                        "Command executed upon container start") {}

    std::string GetDefault() const override {
        if (GetContainer()->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/sbin/init";

        return "";
    }
};

class TEnvProperty : public TListValue, public TContainerValue {
public:
    TEnvProperty() :
        TListValue(PERSISTENT_VALUE),
        TContainerValue(P_ENV,
                        "Container environment variables: <name>=<value>; ...") {}

    TError GetIndexed(const std::string &index, std::string &value) const override {
        auto c = GetContainer();
        TError error;
        TEnv env;

        error = c->GetEnvironment(env);
        if (!error && !env.GetEnv(index, value))
            error = TError(EError::InvalidValue, "Variable " + index + " not defined");
        return error;
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        auto cfg = Get();
        TError error;
        TEnv env;

        error = env.Parse(cfg, true);
        if (!error) {
            error = env.Parse({index + "=" + value}, true);
            env.Format(cfg);
        }
        if (!error)
            error = Set(cfg);
        return error;
    }
};

class TPortoNamespaceProperty : public TStringValue, public TContainerValue {
public:
    TPortoNamespaceProperty() :
        TStringValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_PORTO_NAMESPACE,
                        "Porto containers namespace (container name prefix) (dynamic)") {}
    std::string GetDefault() const override {
        if (GetContainer()->IsRoot())
            return std::string(PORTO_ROOT_CONTAINER) + "/";
        return "";
    }
};

class TRootProperty : public TStringValue, public TContainerValue {
public:
    TRootProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_ROOT,
                     "Container root directory (container will be chrooted into this directory)") {}

    std::string GetDefault() const override {
        return "/";
    }
};

class TRootRdOnlyProperty : public TBoolValue, public TContainerValue {
public:
    TRootRdOnlyProperty() :
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_ROOT_RDONLY,
                        "Mount root directory in read-only mode") {}

    bool GetDefault() const override {
        return false;
    }
};

class TCwdProperty : public TStringValue, public TContainerValue {
public:
    TCwdProperty() :
        TStringValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_CWD,
                        "Container working directory") {}

    std::string GetDefault() const override {
        auto c = GetContainer();

        if (c->IsRoot() || c->IsPortoRoot() || !c->RootPath().IsRoot() ||
                c->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/";

        return c->WorkPath().ToString();
    }
};

class TStdinPathProperty : public TStringValue, public TContainerValue {
public:
    TStdinPathProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_STDIN_PATH,
                        "Container standard input path") {}

    std::string GetDefault() const override {
        return "/dev/null";
    }
};

class TStdoutPathProperty : public TStringValue, public TContainerValue {
public:
    TStdoutPathProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_STDOUT_PATH,
                        "Container standard output path") {}

    std::string GetDefault() const override {
        auto c = GetContainer();

        if (c->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/dev/null";

        return "stdout";
    }
};

class TStderrPathProperty : public TStringValue, public TContainerValue {
public:
    TStderrPathProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_STDERR_PATH,
                        "Container standard error path") {}

    std::string GetDefault() const override {
        auto c = GetContainer();

        if (c->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/dev/null";

        return "stderr";
    }
};

class TStdoutLimitProperty : public TSizeValue, public TContainerValue {
public:
    TStdoutLimitProperty() :
        TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_STDOUT_LIMIT,
                        "Limit returned stdout/stderr size (dynamic)") {}

    uint64_t GetDefault() const override {
        return config().container().stdout_limit();
    }

    TError CheckValue(const uint64_t &value) override {
        uint32_t max = config().container().stdout_limit();

        if (value > max)
            return TError(EError::InvalidValue,
                          "Maximum number of bytes: " +
                          std::to_string(max));

        return TError::Success();
    }
};

class TMemoryGuaranteeProperty : public TSizeValue, public TContainerValue {
public:
    TMemoryGuaranteeProperty() :
        TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_MEM_GUARANTEE,
                        "Guaranteed amount of memory [bytes] (dynamic)") {
        if (!MemorySubsystem.SupportGuarantee())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    TError CheckValue(const uint64_t &value) override {
        auto c = GetContainer();

        uint64_t usage = c->GetRoot()->GetChildrenSum(P_MEM_GUARANTEE, c, value);
        uint64_t total = GetTotalMemory();
        uint64_t reserve = config().daemon().memory_guarantee_reserve();
        if (usage + reserve > total)
            return TError(EError::ResourceNotAvailable,
                          "can't guarantee all available memory: requested " +
                          std::to_string(value) + " (will be " + std::to_string(usage) +
                          " of " + std::to_string(total) + ", reserve " + std::to_string(reserve) + ")");

        return TError::Success();
    }
};

class TMemoryLimitProperty : public TSizeValue, public TContainerValue {
public:
    TMemoryLimitProperty() :
        TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_MEM_LIMIT,
                        "Memory hard limit [bytes] (dynamic)") {}
};

class TAnonLimitProperty : public TSizeValue, public TContainerValue {
public:
    TAnonLimitProperty() :
        TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_ANON_LIMIT,
                        "Anonymous memory limit [bytes] (dynamic)") {
            if (!MemorySubsystem.SupportAnonLimit())
                SetFlag(UNSUPPORTED_FEATURE);
        }
};

class TDirtyLimitProperty : public TSizeValue, public TContainerValue {
public:
    TDirtyLimitProperty() :
        TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_DIRTY_LIMIT,
                        "Dirty file cache limit [bytes] (dynamic)") {
        if (!MemorySubsystem.SupportDirtyLimit())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    uint64_t GetDefault() const override {
        return 0;
    }
};

class TRechargeOnPgfaultProperty : public TBoolValue, public TContainerValue {
public:
    TRechargeOnPgfaultProperty() :
        TBoolValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_RECHARGE_ON_PGFAULT,
                        "Recharge memory on page fault (dynamic)") {
        if (!MemorySubsystem.SupportRechargeOnPgfault())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    bool GetDefault() const override {
        return false;
    }
};

class TCpuPolicyProperty : public TStringValue, public TContainerValue {
public:
    TCpuPolicyProperty() :
        TStringValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_CPU_POLICY,
                        "CPU policy: rt, normal, idle (dynamic)") {}

    std::string GetDefault() const override {
        return "normal";
    }

    TError CheckValue(const std::string &value) override {
        if (value != "normal" && value != "rt" && value != "idle")
            return TError(EError::InvalidValue, "invalid policy");

        return TError::Success();
    }
};

class TCpuLimitProperty : public TCpusValue, public TContainerValue {
public:
    TCpuLimitProperty() :
        TCpusValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_CPU_LIMIT,
                "CPU limit: 0-100.0 [%] | 0.0c-<CPUS>c [cores] (dynamic)") { }

    double GetDefault() const override {
        return GetNumCores();
    }
};

class TCpuGuaranteeProperty : public TCpusValue, public TContainerValue {
public:
    TCpuGuaranteeProperty() :
        TCpusValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_CPU_GUARANTEE,
                "CPU guarantee: 0-100.0 [%] | 0.0c-<CPUS>c [cores] (dynamic)") { }
};

class TIoPolicyProperty : public TStringValue, public TContainerValue {
public:
    TIoPolicyProperty() :
        TStringValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_IO_POLICY,
                        "IO policy: normal, batch (dynamic)") {
        if (!BlkioSubsystem.SupportPolicy())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    std::string GetDefault() const override {
        return "normal";
    }

    TError CheckValue(const std::string &value) override {
        if (value != "normal" && value != "batch")
            return TError(EError::InvalidValue, "invalid policy");

        return TError::Success();
    }
};

class TIoLimitProperty : public TSizeValue, public TContainerValue {
public:
    TIoLimitProperty() :
        TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_IO_LIMIT,
                        "Filesystem bandwidth limit [bytes/s] (dynamic)") {
        if (!MemorySubsystem.SupportIoLimit())
            SetFlag(UNSUPPORTED_FEATURE);
    }
};

class TIopsLimitProperty : public TSizeValue, public TContainerValue {
public:
    TIopsLimitProperty() :
        TSizeValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_IO_OPS_LIMIT,
                        "Filesystem IOPS limit [operations/s] (dynamic)") {
        if (!MemorySubsystem.SupportIoLimit())
            SetFlag(UNSUPPORTED_FEATURE);
    }
};

class TNetGuaranteeProperty : public TMapValue, public TContainerValue {
public:
    TNetGuaranteeProperty() : TMapValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
    TContainerValue(P_NET_GUARANTEE,
            "Guaranteed container network bandwidth: <interface>|default <Bps>;... (dynamic)") { }

    TError CheckValue(const TUintMap &value) override {
        for (auto &kv: value) {
            if (kv.second > NET_MAX_GUARANTEE)
                return TError(EError::InvalidValue, "Net guarantee too large");
        }
        return TError::Success();
    }

    TUintMap GetDefault() const override {
        auto c = GetContainer();
        uint64_t rate = 0;
        if (c->IsRoot() || c->IsPortoRoot())
            rate = NET_MAX_GUARANTEE;
        return TUintMap({{ "default", rate }});
    }
};

class TNetLimitProperty : public TMapValue, public TContainerValue {
public:
    TNetLimitProperty() : TMapValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
    TContainerValue(P_NET_LIMIT,
            "Maximum container network bandwidth: <interface>|default <Bps>;... (dynamic)") { }

    TError CheckValue(const TUintMap &value) override {
        for (auto &kv: value) {
            if (kv.second > NET_MAX_LIMIT)
                return TError(EError::InvalidValue, "Net limit too large");
        }
        return TError::Success();
    }

    TUintMap GetDefault() const override {
        return TUintMap({{ "default", 0 }});
    }
};

class TNetPriorityProperty : public TMapValue, public TContainerValue {
public:
    TNetPriorityProperty() : TMapValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
    TContainerValue(P_NET_PRIO,
            "Container network priority: <interface>|default 0-7;... (dynamic)") { }

    TError CheckValue(const TUintMap &value) override {
        for (auto &kv : value) {
            if (kv.second > 7)
                return TError(EError::InvalidValue, "invalid value");
        }
        return TError::Success();
    }

    TUintMap GetDefault() const override {
        return TUintMap({{"default", NET_DEFAULT_PRIO }});
    }
};

class TRespawnProperty : public TBoolValue, public TContainerValue {
public:
    TRespawnProperty() :
        TBoolValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_RESPAWN,
                        "Automatically respawn dead container (dynamic)") {}

    bool GetDefault() const override {
        return false;
    }
};

class TMaxRespawnsProperty : public TIntValue, public TContainerValue {
public:
    TMaxRespawnsProperty() :
        TIntValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_MAX_RESPAWNS,
                        "Limit respawn count for specific container (dynamic)") {}

    int GetDefault() const override {
        return -1;
    }
};

class TIsolateProperty : public TBoolValue, public TContainerValue {
public:
    TIsolateProperty() :
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_ISOLATE,
                        "Isolate container from parent") {}

    bool GetDefault() const override {
        return true;
    }
};

class TPrivateProperty : public TStringValue, public TContainerValue {
public:
    TPrivateProperty() :
        TStringValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_PRIVATE,
                        "User-defined property (dynamic)") {}

    std::string GetDefault() const override {
        return "";
    }

    TError CheckValue(const std::string &value) override {
        uint32_t max = config().container().private_max();

        if (value.length() > max)
            return TError(EError::InvalidValue, "Value is too long");

        return TError::Success();
    }
};

class TUlimitProperty : public TListValue, public TContainerValue {
    std::map<int,struct rlimit> Rlimit;

public:
    TUlimitProperty() :
        TListValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ULIMIT,
                        "Container resource limits: <type> <soft> <hard>; ... (man 2 getrlimit)") {}

    TError CheckValue(const std::vector<std::string> &lines) override {
        Rlimit.clear();

        static const std::map<std::string,int> nameToIdx = {
            { "as", RLIMIT_AS },
            { "core", RLIMIT_CORE },
            { "cpu", RLIMIT_CPU },
            { "data", RLIMIT_DATA },
            { "fsize", RLIMIT_FSIZE },
            { "locks", RLIMIT_LOCKS },
            { "memlock", RLIMIT_MEMLOCK },
            { "msgqueue", RLIMIT_MSGQUEUE },
            { "nice", RLIMIT_NICE },
            { "nofile", RLIMIT_NOFILE },
            { "nproc", RLIMIT_NPROC },
            { "rss", RLIMIT_RSS },
            { "rtprio", RLIMIT_RTPRIO },
            { "rttime", RLIMIT_RTTIME },
            { "sigpending", RLIMIT_SIGPENDING },
            { "stask", RLIMIT_STACK },
        };

        for (auto &limit : lines) {
            std::vector<std::string> nameval;

            (void)SplitString(limit, ':', nameval);
            if (nameval.size() != 2)
                return TError(EError::InvalidValue, "Invalid limits format");

            std::string name = StringTrim(nameval[0]);
            if (nameToIdx.find(name) == nameToIdx.end())
                return TError(EError::InvalidValue, "Invalid limit " + name);
            int idx = nameToIdx.at(name);

            std::vector<std::string> softhard;
            (void)SplitString(StringTrim(nameval[1]), ' ', softhard);
            if (softhard.size() != 2)
                return TError(EError::InvalidValue, "Invalid limits number for " + name);

            rlim_t soft, hard;
            if (softhard[0] == "unlim" || softhard[0] == "unlimited") {
                soft = RLIM_INFINITY;
            } else {
                TError error = StringToUint64(softhard[0], soft);
                if (error)
                    return TError(EError::InvalidValue, "Invalid soft limit for " + name);
            }

            if (softhard[1] == "unlim" || softhard[1] == "unlimited") {
                hard = RLIM_INFINITY;
            } else {
                TError error = StringToUint64(softhard[1], hard);
                if (error)
                    return TError(EError::InvalidValue, "Invalid hard limit for " + name);
            }

            Rlimit[idx].rlim_cur = soft;
            Rlimit[idx].rlim_max = hard;
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(TTaskEnv &taskEnv) override {
        taskEnv.Rlimit = Rlimit;
        return TError::Success();
    }
};

class THostnameProperty : public TStringValue, public TContainerValue {
public:
    THostnameProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_HOSTNAME,
                        "Container hostname") {}
};

class TBindDnsProperty : public TBoolValue, public TContainerValue {
public:
    TBindDnsProperty() :
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_BIND_DNS,
                        "Bind /etc/resolv.conf and /etc/hosts of host to container") {}

    bool GetDefault() const override {
        auto c = GetContainer();

        auto vmode = c->Prop->Get<int>(P_VIRT_MODE);
        if (vmode == VIRT_MODE_OS)
            return false;

        if (!c->Prop->Get<bool>(P_ISOLATE))
            return false;
        else if (c->Prop->IsDefault(P_ROOT))
            return false;
        else
            return true;
    }
};

class TBindProperty : public TListValue, public TContainerValue {
    std::vector<TBindMap> BindMap;

public:
    TBindProperty() :
        TListValue(PERSISTENT_VALUE),
        TContainerValue(P_BIND,
                        "Share host directories with container: <host_path> <container_path> [ro|rw]; ...") {}

    TError CheckValue(const std::vector<std::string> &lines) override {
        auto c = GetContainer();

        std::vector<TBindMap> bm;

        for (auto &line : lines) {
            std::vector<std::string> tok;
            TBindMap m;

            TError error = SplitEscapedString(line, ' ', tok);
            if (error)
                return error;

            if (tok.size() != 2 && tok.size() != 3)
                return TError(EError::InvalidValue, "Invalid bind in: " + line);

            m.Source = tok[0];
            m.Dest = tok[1];
            m.Rdonly = false;

            if (tok.size() == 3) {
                if (tok[2] == "ro")
                    m.Rdonly = true;
                else if (tok[2] == "rw")
                    m.Rdonly = false;
                else
                    return TError(EError::InvalidValue, "Invalid bind type in: " + line);
            }

            bm.push_back(m);
        }

        BindMap = bm;

        return TError::Success();
    }

    TError PrepareTaskEnv(TTaskEnv &taskEnv) override {
        taskEnv.BindMap = BindMap;
        return TError::Success();
    }
};

class TDefaultGwProperty : public TListValue, public TContainerValue {
public:
    TDefaultGwProperty() :
        TListValue(PERSISTENT_VALUE | HIDDEN_VALUE),
        TContainerValue(P_DEFAULT_GW,
                        "Default gateway: <interface> <ip>; ...") {}

    TError CheckValue(const std::vector<std::string> &lines) override {
        TNetCfg cfg;

        return cfg.ParseGw(lines);
    }
};

class TIpProperty : public TListValue, public TContainerValue {
public:
    TIpProperty() :
        TListValue(PERSISTENT_VALUE),
        TContainerValue(P_IP,
                        "IP configuration: <interface> <ip>/<prefix>; ...") {}

    TError CheckValue(const std::vector<std::string> &lines) override {
        TNetCfg cfg;

        return cfg.ParseIp(lines);
    }
};

class TNetProperty : public TListValue, public TContainerValue {
public:
    TNetProperty() :
        TListValue(PERSISTENT_VALUE),
        TContainerValue(P_NET,
                        "Container network settings: "
                        "none | "
                        "inherited | "
                        "host [interface] | "
                        "container <name> | "
                        "macvlan <master> <name> [bridge|private|vepa|passthru] [mtu] [hw] | "
                        "ipvlan <master> <name> [l2|l3] [mtu] | "
                        "veth <name> <bridge> [mtu] [hw] | "
                        "L3 <name> [master] | "
                        "NAT [name] | "
                        "MTU <name> <mtu> | "
                        "autoconf <name> | "
                        "netns <name>") {}

    TStrList GetDefault() const override {
        auto c = GetContainer();

        if (c->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            return TStrList{ "none" };

        return TStrList{ "inherited" };
    }

    TError CheckValue(const std::vector<std::string> &lines) override {
        TNetCfg cfg;

        return cfg.ParseNet(lines);
    }
};

class TNetTosProperty : public TUintValue, public TContainerValue {
public:
    TNetTosProperty() :
        TUintValue(PERSISTENT_VALUE),
        TContainerValue(P_NET_TOS,
                        "IP TOS") {
        SetFlag(UNSUPPORTED_FEATURE);
    }
};

class TDevicesProperty : public TListValue, public TContainerValue {
public:
    TDevicesProperty():
        TListValue(PERSISTENT_VALUE),
        TContainerValue(P_DEVICES, "Devices that container can access: "
                "<device> [r][w][m][-] [name] [mode] [user] [group]; ...") {}
};

struct TCapDesc {
    int id;
    int flags;
};

#define RESTRICTED_CAP 1

#ifndef CAP_AUDIT_READ
#define CAP_AUDIT_READ 37
#define CAP_BLOCK_SUSPEND 36
#endif

class TCapabilitiesProperty : public TListValue, public TContainerValue {
    uint64_t Caps;
    const std::map<std::string, TCapDesc> Supported = {
        { "AUDIT_READ",         { CAP_AUDIT_READ, 0 } },
        { "CHOWN",              { CAP_CHOWN, RESTRICTED_CAP } },
        { "DAC_OVERRIDE",       { CAP_DAC_OVERRIDE, RESTRICTED_CAP } },
        { "DAC_READ_SEARCH",    { CAP_DAC_READ_SEARCH, 0 } },
        { "FOWNER",             { CAP_FOWNER, RESTRICTED_CAP } },
        { "FSETID",             { CAP_FSETID, RESTRICTED_CAP } },
        { "KILL",               { CAP_KILL, RESTRICTED_CAP } },
        { "SETGID",             { CAP_SETGID, RESTRICTED_CAP } },
        { "SETUID",             { CAP_SETUID, RESTRICTED_CAP } },
        { "SETPCAP",            { CAP_SETPCAP, 0 } },
        { "LINUX_IMMUTABLE",    { CAP_LINUX_IMMUTABLE, 0 } },
        { "NET_BIND_SERVICE",   { CAP_NET_BIND_SERVICE, RESTRICTED_CAP } },
        { "NET_BROADCAST",      { CAP_NET_BROADCAST, 0 } },
        { "NET_ADMIN",          { CAP_NET_ADMIN, RESTRICTED_CAP } },
        { "NET_RAW",            { CAP_NET_RAW, RESTRICTED_CAP } },
        { "IPC_LOCK",           { CAP_IPC_LOCK, RESTRICTED_CAP } },
        { "IPC_OWNER",          { CAP_IPC_OWNER, 0 } },
        { "SYS_MODULE",         { CAP_SYS_MODULE, 0 } },
        { "SYS_RAWIO",          { CAP_SYS_RAWIO, 0 } },
        { "SYS_CHROOT",         { CAP_SYS_CHROOT, RESTRICTED_CAP } },
        { "SYS_PTRACE",         { CAP_SYS_PTRACE, 0 } },
        { "SYS_PACCT",          { CAP_SYS_PACCT, 0 } },
        { "SYS_ADMIN",          { CAP_SYS_ADMIN, 0 } },
        { "SYS_BOOT",           { CAP_SYS_BOOT, 0 } },
        { "SYS_NICE",           { CAP_SYS_NICE, 0 } },
        { "SYS_RESOURCE",       { CAP_SYS_RESOURCE, RESTRICTED_CAP } },
        { "SYS_TIME",           { CAP_SYS_TIME, 0 } },
        { "SYS_TTY_CONFIG",     { CAP_SYS_TTY_CONFIG, 0 } },
        { "MKNOD",              { CAP_MKNOD, RESTRICTED_CAP } },
        { "LEASE",              { CAP_LEASE, 0 } },
        { "AUDIT_WRITE",        { CAP_AUDIT_WRITE, RESTRICTED_CAP } },
        { "AUDIT_CONTROL",      { CAP_AUDIT_CONTROL, 0 } },
        { "SETFCAP",            { CAP_SETFCAP, 0 } },
        { "MAC_OVERRIDE",       { CAP_MAC_OVERRIDE, 0 } },
        { "MAC_ADMIN",          { CAP_MAC_ADMIN, 0 } },
        { "SYSLOG",             { CAP_SYSLOG, 0 } },
        { "WAKE_ALARM",         { CAP_WAKE_ALARM, 0 } },
        { "BLOCK_SUSPEND",      { CAP_BLOCK_SUSPEND, 0 } },
    };

public:
    TCapabilitiesProperty() :
        TListValue(PERSISTENT_VALUE | SUPERUSER_PROPERTY | HIDDEN_VALUE),
        TContainerValue(P_CAPABILITIES,
                        "Limit container capabilities: list of capabilities without CAP_ prefix (man 7 capabilities)") {}

    TStrList GetDefault() const override {
        TStrList v;
        auto c = GetContainer();

        bool root = c->OwnerCred.IsRootUser();
        auto vmode = c->Prop->Get<int>(P_VIRT_MODE);
        bool restricted = vmode == VIRT_MODE_OS;

        for (const auto &kv : Supported)
            if ((root || (restricted && kv.second.flags & RESTRICTED_CAP))
                && kv.second.id <= LastCapability)
                v.push_back(kv.first);
        return v;
    }

    TError CheckValue(const std::vector<std::string> &lines) override {
        uint64_t allowed = 0;

        for (auto &line: lines) {
            if (Supported.find(line) == Supported.end())
                return TError(EError::InvalidValue,
                              "Unsupported capability " + line);

            if (Supported.at(line).id > LastCapability)
                return TError(EError::InvalidValue,
                              "Unsupported kernel capability " + line);

            allowed |= (1ULL << Supported.at(line).id);
        }

        Caps = allowed;

        return TError::Success();
    }

    TError PrepareTaskEnv(TTaskEnv &taskEnv) override {
        taskEnv.Caps = Caps;
        return TError::Success();
    }
};

class TVirtModeProperty : public TIntValue, public TContainerValue {
public:
    TVirtModeProperty() :
        TIntValue(PERSISTENT_VALUE),
        TContainerValue(P_VIRT_MODE,
                        "Virtualization mode: os|app") {}

    std::string ToString(const int &value) const override {
        if (value == VIRT_MODE_OS)
            return "os";
        else if (value == VIRT_MODE_APP)
            return "app";
        else
            return "unknown " + std::to_string(value);
    }

    TError FromString(const std::string &value, int &mode) const override {
        if (value == "app") {
            mode = VIRT_MODE_APP;
        } else if (value == "os") {
            mode = VIRT_MODE_OS;
        } else {
            return TError(EError::InvalidValue, std::string("Unsupported ") + P_VIRT_MODE + ": " + value);
        }
        return TError::Success();
    }
};

class TAgingTimeProperty : public TUintValue, public TContainerValue {
public:
    TAgingTimeProperty() :
        TUintValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_AGING_TIME,
                        "After given number of seconds container in dead state is automatically removed (dynamic)") {}

    uint64_t GetDefault() const override {
        return config().container().default_aging_time_s();
    }
};

class TEnablePortoProperty : public TBoolValue, public TContainerValue {
public:
    TEnablePortoProperty() :
        TBoolValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_ENABLE_PORTO,
                        "Allow container communication with porto (dynamic)") {}

    bool GetDefault() const override {
        for (auto c = GetContainer(); c; c = c->GetParent()) {
            if (c->Prop->HasValue(P_ENABLE_PORTO))
                return c->Prop->Get<bool>(P_ENABLE_PORTO);
        }
        return true;
    }

    TError CheckValue(const bool &value) override {
        if (value == true) {
            auto c = GetContainer();
            auto p = c->GetParent();
            if (p && !p->Prop->Get<bool>(P_ENABLE_PORTO))
                return TError(EError::InvalidValue, "Porto disabled in parent container");
        }
        return TError::Success();
    }
};

class TResolvConfProperty : public TListValue, public TContainerValue {
public:
    TResolvConfProperty() :
        TListValue(PERSISTENT_VALUE),
        TContainerValue(P_RESOLV_CONF,
                        "DNS resolver configuration: <resolv.conf option>;...") {}

    TError PrepareTaskEnv(TTaskEnv &taskEnv) override {
        if (HasValue()) {
            if (taskEnv.Root.IsRoot())
                return TError(EError::InvalidValue,
                        "resolv_conf requires separate root");
            taskEnv.BindDns = false;
            auto lines = Get();
            for (auto &line: lines)
                taskEnv.ResolvConf += line + "\n";
        }
        return TError::Success();
    }
};

class TWeakProperty : public TBoolValue, public TContainerValue {
public:
    TWeakProperty() :
        TBoolValue(PERSISTENT_VALUE | DYNAMIC_VALUE),
        TContainerValue(P_WEAK,
                        "Destroy container when client disconnects (dynamic)") {}

    bool GetDefault() const override {
        return false;
    }
};

class TRawIdProperty : public TIntValue, public TContainerValue {
public:
    TRawIdProperty() :
        TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_ID, "") {}
    int GetDefault() const override { return -1; }
};

class TRawRootPidProperty : public TIntListValue, public TContainerValue {
public:
    TRawRootPidProperty() :
        TIntListValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_ROOT_PID, "") {}
};

class TRawLoopDevProperty : public TIntValue, public TContainerValue {
public:
    TRawLoopDevProperty() :
        TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_LOOP_DEV, "") {}
    int GetDefault() const override { return -1; }
};

class TRawNameProperty : public TStringValue, public TContainerValue {
public:
    TRawNameProperty() :
        TStringValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_NAME, "") {}
};

class TRawStartTimeProperty : public TUintValue, public TContainerValue {
public:
    TRawStartTimeProperty() :
        TUintValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_START_TIME, "") {}
};

class TRawDeathTimeProperty : public TUintValue, public TContainerValue {
public:
    TRawDeathTimeProperty() :
        TUintValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_DEATH_TIME, "") {}
};

void RegisterProperties(std::shared_ptr<TRawValueMap> m,
                        std::shared_ptr<TContainer> c) {
    const std::vector<TValue *> properties = {
        new TCommandProperty,
        new TEnvProperty,
        new TPortoNamespaceProperty,
        new TRootProperty,
        new TRootRdOnlyProperty,
        new TCwdProperty,
        new TStdinPathProperty,
        new TStdoutPathProperty,
        new TStderrPathProperty,
        new TStdoutLimitProperty,
        new TMemoryGuaranteeProperty,
        new TMemoryLimitProperty,
        new TAnonLimitProperty,
        new TDirtyLimitProperty,
        new TRechargeOnPgfaultProperty,
        new TCpuPolicyProperty,
        new TCpuLimitProperty,
        new TCpuGuaranteeProperty,
        new TIoPolicyProperty,
        new TIoLimitProperty,
        new TIopsLimitProperty,
        new TNetGuaranteeProperty,
        new TNetLimitProperty,
        new TNetPriorityProperty,
        new TRespawnProperty,
        new TMaxRespawnsProperty,
        new TIsolateProperty,
        new TPrivateProperty,
        new TUlimitProperty,
        new THostnameProperty,
        new TBindDnsProperty,
        new TBindProperty,
        new TNetProperty,
        new TNetTosProperty,
        new TDevicesProperty,
        new TCapabilitiesProperty,
        new TIpProperty,
        new TDefaultGwProperty,
        new TVirtModeProperty,
        new TAgingTimeProperty,
        new TEnablePortoProperty,
        new TResolvConfProperty,
        new TWeakProperty,

        new TRawIdProperty,
        new TRawRootPidProperty,
        new TRawLoopDevProperty,
        new TRawNameProperty,
        new TRawStartTimeProperty,
        new TRawDeathTimeProperty,
    };

    for (auto p : properties)
        AddContainerValue(m, c, p);
}

/*
 * Note for other properties:
 * Dead state 2-line check is mandatory for all properties
 * Some properties require to check if the property is supported
 * Some properties may forbid changing it in runtime
 * Of course, some properties can be read-only
 */

TError TContainerUser::Set(const std::string &username) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    uid_t new_uid;
    error = UserId(username, new_uid);
    if (error)
        return error;

    TCred &owner = CurrentContainer->OwnerCred;
    TCred new_user(new_uid, owner.Gid);
    error = FindGroups(username, new_user.Gid, new_user.Groups);
    if (error)
        return error;

    TCred &cur_user = CurrentClient->Cred;

    if (cur_user.CanControl(new_user)) {
        owner.Uid = new_user.Uid;
        owner.Groups.clear();
        owner.Groups.insert(owner.Groups.end(), new_user.Groups.begin(),
                            new_user.Groups.end());

        CurrentContainer->PropMask |= USER_SET;

        return TError::Success();
    }

    return TError(EError::Permission,
                  "Client is not allowed to set user : " + username);
}

TError TContainerUser::Get(std::string &value) {
    value = UserName(CurrentContainer->OwnerCred.Uid);

    return TError::Success();
}

TError TContainerGroup::Set(const std::string &groupname) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    gid_t new_gid;
    error = GroupId(groupname, new_gid);
    if (error)
        return error;

    if (CurrentClient->Cred.IsRootUser()) {
        CurrentContainer->OwnerCred.Gid = new_gid;
        CurrentContainer->PropMask |= GROUP_SET;

        return TError::Success();
    }

    if (CurrentContainer->OwnerCred.IsMemberOf(new_gid)) {
        CurrentContainer->OwnerCred.Gid = new_gid;
        CurrentContainer->PropMask |= GROUP_SET;

        return TError::Success();
    }

    return TError(EError::Permission, "Desired group : " + groupname +
                  " isn't in current user supplementary group list");
}
    TError SetValue(const std::string &value);

TError TContainerGroup::Get(std::string &value) {
    value = GroupName(CurrentContainer->OwnerCred.Gid);

    return TError::Success();
}

void InitContainerProperties(void) {
    ContainerPropMap[ContainerUser.Name] = &ContainerUser;
    ContainerPropMap[ContainerGroup.Name] = &ContainerGroup;
}

TError TContainerProperty::IsAliveAndStopped(void) {
    auto state = CurrentContainer->GetState();

    if (state == EContainerState::Dead)
        return TError(EError::InvalidState,
                      "Cannot change property while in the dead state");

    if (state != EContainerState::Stopped &&
        state != EContainerState::Unknown)
        return TError(EError::InvalidState,
                "Cannot change property in runtime");

    return TError::Success();
}
