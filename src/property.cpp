#include "property.hpp"
#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "container_value.hpp"
#include "network.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"

extern "C" {
#include <linux/capability.h>
}

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

bool TPropertyMap::HasState(const std::string &property, EContainerState state) const {
    auto prop = Find(property);
    if (!prop) {
        L_ERR() << TError(EError::Unknown, "Invalid property " + property) << std::endl;
        return false;
    }

    auto cv = ToContainerValue(prop);
    auto valueState = cv->GetState();

    return valueState.find(state) != valueState.end();
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

static std::set<EContainerState> staticProperty = {
    EContainerState::Stopped,
};

static std::set<EContainerState> dynamicProperty = {
    EContainerState::Stopped,
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Meta,
};

static std::set<EContainerState> anyState = {
    EContainerState::Stopped,
    EContainerState::Dead,
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Meta
};

class TCommandProperty : public TStringValue, public TContainerValue {
public:
    TCommandProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_COMMAND,
                        "Command executed upon container start",
                        staticProperty) {}

    std::string GetDefault() const override {
        if (GetContainer()->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            return "/sbin/init";

        return "";
    }
};

class TUserProperty : public TStringValue, public TContainerValue {
public:
    TUserProperty() :
        TStringValue(SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_USER,
                        "Start command with given user",
                        staticProperty) {}

    TError CheckValue(const std::string &value) override {
        auto container = GetContainer();
        TUser user(value);
        TError error = user.Load();
        if (!error)
            container->OwnerCred.Uid = user.GetId();
        return error;
    }
};

class TGroupProperty : public TStringValue, public TContainerValue {
public:
    TGroupProperty() :
        TStringValue(SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_GROUP,
                        "Start command with given group",
                        staticProperty) {}

    TError CheckValue(const std::string &value) override {
        auto container = GetContainer();
        TGroup group(value);
        TError error = group.Load();
        if (!error)
            container->OwnerCred.Gid = group.GetId();
        return error;
    }
};

class TEnvProperty : public TListValue, public TContainerValue {
public:
    TEnvProperty() :
        TListValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ENV,
                        "Container environment variables: <name>=<value>; ...",
                        staticProperty) {}
};

class TPortoNamespaceProperty : public TStringValue, public TContainerValue {
public:
    TPortoNamespaceProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_PORTO_NAMESPACE,
                        "Porto containers namespace",
                        staticProperty) {}
};

class TRootProperty : public TStringValue, public TContainerValue {
public:
    TRootProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_ROOT,
                     "Container root directory (container will be chrooted into this directory)",
                     staticProperty) {}

    std::string GetDefault() const override {
        return "/";
    }

    TError CheckValue(const std::string &value) override {
        auto c = GetContainer();

        if (c->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS) {
            TPath root(value);
            TPath realRoot("/");

            if (!GetContainer()->OwnerCred.IsPrivileged() &&
                root.IsDirectory() && root.GetDev() == realRoot.GetDev())
                return TError(EError::Permission, "Can't start OS container on the same mount point as /");
        }

        return TError::Success();
    }
};

class TRootRdOnlyProperty : public TBoolValue, public TContainerValue {
public:
    TRootRdOnlyProperty() :
        TBoolValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ROOT_RDONLY,
                        "Mount root directory in read-only mode",
                        staticProperty) {}

    bool GetDefault() const override {
        return false;
    }
};

class TCwdProperty : public TStringValue, public TContainerValue {
public:
    TCwdProperty() :
        TStringValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_CWD,
                        "Container working directory",
                        staticProperty) {}

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
                        "Container standard input path",
                        staticProperty) {}

    std::string GetDefault() const override {
        return "/dev/null";
    }
};

class TStdTypeProperty : public TStringValue {
public:
    TStdTypeProperty() : TStringValue(PERSISTENT_VALUE | HIDDEN_VALUE) {}

    std::string GetDefault() const override {
        return STD_TYPE_FILE;
    }

    TError CheckValue(const std::string &value) override {
        if (value == STD_TYPE_FILE || value == STD_TYPE_FIFO || value == STD_TYPE_PTY)
            return TError::Success();
        else
            return TError(EError::InvalidValue, "Invalid std type value");
    }
};

class TStdInTypeProperty : public TStdTypeProperty, public TContainerValue {
public:
    TStdInTypeProperty() : TContainerValue(P_STDIN_TYPE,
            "Container standard input type [file, pipe, pty]",
            staticProperty) { }
};

class TStdOutTypeProperty : public TStdTypeProperty, public TContainerValue {
public:
    TStdOutTypeProperty() : TContainerValue(P_STDOUT_TYPE,
            "Container standard output type [file, pipe, pty]",
            staticProperty) { }
};

class TStdErrTypeProperty : public TStdTypeProperty, public TContainerValue {
public:
    TStdErrTypeProperty() : TContainerValue(P_STDERR_TYPE,
            "Container standard error output type [file, pipe, pty]",
            staticProperty) { }
};

class TStdoutPathProperty : public TStringValue, public TContainerValue {
public:
    TStdoutPathProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_STDOUT_PATH,
                        "Container standard output path",
                        staticProperty) {}

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
                        "Container standard error path",
                        staticProperty) {}

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
        TSizeValue(PERSISTENT_VALUE),
        TContainerValue(P_STDOUT_LIMIT,
                        "Return no more than given number of bytes from standard output/error",
                        anyState) {}

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
        TSizeValue(PERSISTENT_VALUE),
        TContainerValue(P_MEM_GUARANTEE,
                        "Guaranteed amount of memory [bytes]",
                        dynamicProperty) {
        if (!MemorySubsystem.SupportGuarantee())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    TError CheckValue(const uint64_t &value) override {
        auto c = GetContainer();

        if (!c->ValidHierarchicalProperty(P_MEM_GUARANTEE, value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

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
        TSizeValue(PERSISTENT_VALUE),
        TContainerValue(P_MEM_LIMIT,
                        "Memory hard limit [bytes]",
                        dynamicProperty) {}

    TError CheckValue(const uint64_t &value) override {
        if (!GetContainer()->ValidHierarchicalProperty(P_MEM_LIMIT, value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        return TError::Success();
    }
};

class TDirtyLimitProperty : public TSizeValue, public TContainerValue {
public:
    TDirtyLimitProperty() :
        TSizeValue(PERSISTENT_VALUE),
        TContainerValue(P_DIRTY_LIMIT,
                        "Dirty file cache limit [bytes]",
                        dynamicProperty) {
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
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_RECHARGE_ON_PGFAULT,
                        "Recharge memory on page fault",
                        dynamicProperty) {
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
        TStringValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_CPU_POLICY,
                        "CPU policy: rt, normal, idle",
                        dynamicProperty) {}

    std::string GetDefault() const override {
        return "normal";
    }

    TError CheckValue(const std::string &value) override {
        if (value != "normal" && value != "rt" && value != "idle")
            return TError(EError::InvalidValue, "invalid policy");

        if (value == "rt") {
            if (!CpuSubsystem.SupportSmart())
                return TError(EError::NotSupported, "invalid kernel (no Yandex extensions)");
        }

        if (value == "idle")
            return TError(EError::NotSupported, "not implemented");

        return TError::Success();
    }
};

static double ParseCpuLimit(const std::string &str) {
    size_t pos = 0;
    double v = stod(str, &pos);
    if (pos > 0 && pos < str.length() && str[pos] == 'c')
        v = v * 100 / GetNumCores();
    return v;
}

class TCpuLimitProperty : public TDoubleValue, public TContainerValue {
public:
    TCpuLimitProperty() :
        TDoubleValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_CPU_LIMIT,
                        "CPU limit: 0-100.0 [%] | 0.0c-<CPUS>c [cores]",
                        dynamicProperty) {
        if (!CpuSubsystem.SupportLimit())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    double GetDefault() const override {
        return 100;
    }

    std::string ToString(const double &value) const override {
        return StringFormat("%lgc", value / 100 * GetNumCores());
    }

    TError FromString(const std::string &str, double &limit) const override {
        try {
            limit = ParseCpuLimit(str);
            if (limit < 0 || limit > 100)
                return TError(EError::InvalidValue,
                        "cpu limit out of range 0-100: " + std::to_string(limit));

            if (!GetContainer()->ValidHierarchicalProperty(P_CPU_LIMIT, limit))
                return TError(EError::InvalidValue, "invalid hierarchical value");

            return TError::Success();
        } catch (...) {
            return TError(EError::InvalidValue, "invalid value");
        }
    }
};

class TCpuGuaranteeProperty : public TDoubleValue, public TContainerValue {
public:
    TCpuGuaranteeProperty() :
        TDoubleValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_CPU_GUARANTEE,
                        "CPU guarantee: 0-100.0 [%] | 0.0c-<CPUS>c [cores]",
                        dynamicProperty) {
        if (!CpuSubsystem.SupportGuarantee())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    std::string ToString(const double &value) const override {
        return StringFormat("%lgc", value / 100 * GetNumCores());
    }

    TError FromString(const std::string &str, double &limit) const override {
        try {
            limit = ParseCpuLimit(str);
            if (limit < 0 || limit > 100)
                return TError(EError::InvalidValue,
                        "cpu guarantee out of range 0-100: " + std::to_string(limit));

            return TError::Success();
        } catch (...) {
            return TError(EError::InvalidValue, "invalid value");
        }
    }
};

class TIoPolicyProperty : public TStringValue, public TContainerValue {
public:
    TIoPolicyProperty() :
        TStringValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_IO_POLICY,
                        "IO policy: normal, batch",
                        dynamicProperty) {
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
        TSizeValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_IO_LIMIT,
                        "Filesystem bandwidth limit [bytes/s]",
                        dynamicProperty) {
        if (!MemorySubsystem.SupportIoLimit())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    uint64_t GetDefault() const override {
        return 0;
    }
};

class TIopsLimitProperty : public TSizeValue, public TContainerValue {
public:
    TIopsLimitProperty() :
        TSizeValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_IO_OPS_LIMIT,
                        "Filesystem IOPS limit [operations/s]",
                        dynamicProperty) {
        if (!MemorySubsystem.SupportIoLimit())
            SetFlag(UNSUPPORTED_FEATURE);
    }

    uint64_t GetDefault() const override {
        return 0;
    }
};

class TNetMapValue : public TMapValue, public TContainerValue {
    uint64_t Def, RootDef;

public:
    virtual uint32_t GetDef() const { return 0; }
    virtual uint32_t GetRootDef() const { return 0; }

    TNetMapValue(
           const char *name,
           const char *desc,
           const int flags,
           const std::set<EContainerState> &state) :
        TMapValue(flags),
        TContainerValue(name, desc, state) {}

    TUintMap GetDefault() const override {
        auto c = GetContainer();
        uint64_t def =  (c->IsRoot() || c->IsPortoRoot()) ? GetRootDef() : GetDef();
        TUintMap m;
        m["default"] = def;
        return m;
    }

    virtual TError Set(const TUintMap &value) override {
        /* Merge, not replace */
        TUintMap tmp = Get();

        for (const auto &iter : value) {
            if (iter.second == NET_MAP_WHITEOUT &&
                iter.first != "default") {
                tmp.erase(iter.first);
                continue;
            }
            tmp[iter.first] = iter.second;
        }

        return TStoredValue::Set(tmp);
    }
};

class TNetGuaranteeProperty : public TNetMapValue {
public:
    TNetGuaranteeProperty() :
        TNetMapValue(P_NET_GUARANTEE,
                     "Guaranteed container network bandwidth [bytes/s] (max 32Gbps)",
                     PERSISTENT_VALUE | PARENT_DEF_PROPERTY,
                     staticProperty) {}
    uint32_t GetDef() const override { return config().network().default_guarantee(); }
    uint32_t GetRootDef() const override { return config().network().default_max_guarantee(); }
    TError CheckValue(const TUintMap &value) override {
        for (auto &kv : value) {
            if (kv.second == NET_MAP_WHITEOUT && kv.first != "default")
                continue;
            if (kv.second > NET_MAX_GUARANTEE)
                return TError(EError::InvalidValue, "Net guarantee too large");
        }

        return TError::Success();
    }
};

class TNetLimitProperty : public TNetMapValue {
public:
    TNetLimitProperty() :
        TNetMapValue(P_NET_LIMIT,
                     "Maximum container network bandwidth [bytes/s] (max 32Gbps)",
                     PERSISTENT_VALUE | PARENT_DEF_PROPERTY,
                     staticProperty) {}
    uint32_t GetDef() const override { return config().network().default_limit(); }
    uint32_t GetRootDef() const override { return config().network().default_max_guarantee(); }
    TError CheckValue(const TUintMap &value) override {
        for (auto &kv : value) {
            if (kv.second == NET_MAP_WHITEOUT && kv.first != "default")
                continue;
            if (kv.second > NET_MAX_LIMIT)
                return TError(EError::InvalidValue, "Net limit too large");
        }

        return TError::Success();
    }
};

class TNetPriorityProperty : public TNetMapValue {
public:
    TNetPriorityProperty() :
        TNetMapValue(P_NET_PRIO,
                     "Container network priority: 0-7",
                     PERSISTENT_VALUE | PARENT_DEF_PROPERTY,
                     staticProperty) {}
    uint32_t GetDef() const override { return config().network().default_prio(); }
    uint32_t GetRootDef() const override { return config().network().default_prio(); }

    TError CheckValue(const TUintMap &value) override {
        for (auto &kv : value) {
            if (kv.second == NET_MAP_WHITEOUT && kv.first != "default")
                continue;
            if (kv.second > 7)
                return TError(EError::InvalidValue, "invalid value");
        }

        return TError::Success();
    }
};

class TRespawnProperty : public TBoolValue, public TContainerValue {
public:
    TRespawnProperty() :
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_RESPAWN,
                        "Automatically respawn dead container",
                        staticProperty) {}

    bool GetDefault() const override {
        return false;
    }
};

class TMaxRespawnsProperty : public TIntValue, public TContainerValue {
public:
    TMaxRespawnsProperty() :
        TIntValue(PERSISTENT_VALUE),
        TContainerValue(P_MAX_RESPAWNS,
                        "Limit respawn count for specific container",
                        staticProperty) {}

    int GetDefault() const override {
        return -1;
    }
};

class TIsolateProperty : public TBoolValue, public TContainerValue {
public:
    TIsolateProperty() :
        TBoolValue(PERSISTENT_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_ISOLATE,
                        "Isolate container from parent",
                        staticProperty) {}

    bool GetDefault() const override {
        return true;
    }
};

class TPrivateProperty : public TStringValue, public TContainerValue {
public:
    TPrivateProperty() :
        TStringValue(PERSISTENT_VALUE),
        TContainerValue(P_PRIVATE,
                        "User-defined property",
                        dynamicProperty) {}

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
                        "Container resource limits: <type> <soft> <hard>; ... (man 2 getrlimit)",
                        staticProperty) {}

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
                        "Container hostname",
                        staticProperty) {}
};

class TBindDnsProperty : public TBoolValue, public TContainerValue {
public:
    TBindDnsProperty() :
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_BIND_DNS,
                        "Bind /etc/resolv.conf and /etc/hosts of host to container",
                        staticProperty) {}

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
                        "Share host directories with container: <host_path> <container_path> [ro|rw]; ...",
                        staticProperty) {}

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
                        "Default gateway: <interface> <ip>; ...",
                        staticProperty) {}

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
                        "IP configuration: <interface> <ip>/<prefix>; ...",
                        staticProperty) {}

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
                        "L3 [name] [mtu] | "
                        "NAT [name] [mtu] | "
                        "netns <name>",
                        staticProperty) {}

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
                        "IP TOS",
                        staticProperty) {
        SetFlag(UNSUPPORTED_FEATURE);
    }
};

class TAllowedDevicesProperty : public TListValue, public TContainerValue {
public:
    TAllowedDevicesProperty() :
        TListValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE | HIDDEN_VALUE | OS_MODE_PROPERTY),
        TContainerValue(P_ALLOWED_DEVICES,
                        "Devices that container can create/read/write: <c|b|a> <maj>:<min> [r][m][w]; ...",
                        staticProperty) {}

    TStrList GetDefault() const override {
        auto c = GetContainer();

        if (c->IsRoot() || c->IsPortoRoot())
            return TStrList{ "a *:* rwm" };

        if (c->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            return TStrList {
                "c 1:3 rwm",    // /dev/null
                "c 1:5 rwm",    // /dev/zero
                "c 1:7 rwm",    // /dev/full
                "c 1:8 rwm",    // /dev/random
                "c 1:9 rwm",    // /dev/urandom
                "c 5:0 rwm",    // /dev/tty
                "c 5:2 rwm",    // /dev/ptmx
                "c 136:* rw",   // /dev/pts/*
                "c 254:0 rm",   // /dev/rtc0
                "c 10:237 rmw", // /dev/loopcontrol FIXME
                "b 7:* rmw"     // /dev/loop*       FIXME
            };

        return c->GetParent()->Prop->Get<TStrList>(P_ALLOWED_DEVICES);
    }
};

struct TCapDesc {
    uint64_t id;
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
        { "MKNOD",              { CAP_MKNOD, 0 } },
        { "LEASE",              { CAP_LEASE, 0 } },
        { "AUDIT_WRITE",        { CAP_AUDIT_WRITE, 0 } },
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
                        "Limit container capabilities: list of capabilities without CAP_ prefix (man 7 capabilities)",
                        staticProperty) {}

    uint64_t GetLastCap() const {
        uint64_t lastCap = 36;
        TFile f("/proc/sys/kernel/cap_last_cap");
        TError error = f.AsUint64(lastCap);
        if (error)
            L_WRN() << "Can't read /proc/sys/kernel/cap_last_cap, assuming 3.10 kernel" << std::endl;
        return lastCap;
    }

    TStrList GetDefault() const override {
        TStrList v;
        auto c = GetContainer();

        bool root = c->OwnerCred.IsRootUser();
        auto vmode = c->Prop->Get<int>(P_VIRT_MODE);
        bool restricted = vmode == VIRT_MODE_OS;

        uint64_t lastCap = GetLastCap();
        for (const auto &kv : Supported)
            if ((root || (restricted && kv.second.flags & RESTRICTED_CAP))
                && kv.second.id <= lastCap)
                v.push_back(kv.first);
        return v;
    }

    TError CheckValue(const std::vector<std::string> &lines) override {
        uint64_t allowed = 0;

        uint64_t lastCap = GetLastCap();
        for (auto &line: lines) {
            if (Supported.find(line) == Supported.end())
                return TError(EError::InvalidValue,
                              "Unsupported capability " + line);

            if (Supported.at(line).id > lastCap)
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
        TIntValue(PERSISTENT_VALUE | RESTROOT_PROPERTY),
        TContainerValue(P_VIRT_MODE,
                        "Virtualization mode: os|app",
                        staticProperty) {}

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
        TUintValue(PERSISTENT_VALUE),
        TContainerValue(P_AGING_TIME,
                        "After given number of seconds container in dead state is automatically removed",
                        staticProperty) {}

    uint64_t GetDefault() const override {
        return config().container().default_aging_time_s();
    }
};

class TEnablePortoProperty : public TBoolValue, public TContainerValue {
public:
    TEnablePortoProperty() :
        TBoolValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_ENABLE_PORTO,
                        "Allow container communication with porto",
                        staticProperty) {}

    bool GetDefault() const override {
        return true;
    }

    TError CheckValue(const bool &value) override {
        if (value == false) {
            auto c = GetContainer();
            if (c->Prop->Get<std::string>(P_ROOT) == "/")
                return TError(EError::InvalidValue, "Can't disable porto socket when container is not isolated");
        }

        return TError::Success();
    }
};

class TResolvConfProperty : public TListValue, public TContainerValue {
public:
    TResolvConfProperty() :
        TListValue(PARENT_DEF_PROPERTY | PERSISTENT_VALUE),
        TContainerValue(P_RESOLV_CONF,
                        "DNS resolver configuration: <resolv.conf option>;...",
                        staticProperty) {}

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
        TBoolValue(PERSISTENT_VALUE),
        TContainerValue(P_WEAK,
                        "Destroy container when client disconnects",
                        anyState) {}

    bool GetDefault() const override {
        return false;
    }
};

class TRawIdProperty : public TIntValue, public TContainerValue {
public:
    TRawIdProperty() :
        TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_ID, "", anyState) {}
    int GetDefault() const override { return -1; }
};

class TRawRootPidProperty : public TIntListValue, public TContainerValue {
public:
    TRawRootPidProperty() :
        TIntListValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_ROOT_PID, "", anyState) {}
};

class TRawLoopDevProperty : public TIntValue, public TContainerValue {
public:
    TRawLoopDevProperty() :
        TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_LOOP_DEV, "", anyState) {}
    int GetDefault() const override { return -1; }
};

class TRawNameProperty : public TStringValue, public TContainerValue {
public:
    TRawNameProperty() :
        TStringValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_NAME, "", anyState) {}
};

class TRawStartTimeProperty : public TUintValue, public TContainerValue {
public:
    TRawStartTimeProperty() :
        TUintValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_START_TIME, "", anyState) {}
};

class TRawDeathTimeProperty : public TUintValue, public TContainerValue {
public:
    TRawDeathTimeProperty() :
        TUintValue(HIDDEN_VALUE | PERSISTENT_VALUE),
        TContainerValue(P_RAW_DEATH_TIME, "", anyState) {}
};

void RegisterProperties(std::shared_ptr<TRawValueMap> m,
                        std::shared_ptr<TContainer> c) {
    const std::vector<TValue *> properties = {
        new TCommandProperty,
        new TUserProperty,
        new TGroupProperty,
        new TEnvProperty,
        new TPortoNamespaceProperty,
        new TRootProperty,
        new TRootRdOnlyProperty,
        new TCwdProperty,
        new TStdInTypeProperty,
        new TStdOutTypeProperty,
        new TStdErrTypeProperty,
        new TStdinPathProperty,
        new TStdoutPathProperty,
        new TStderrPathProperty,
        new TStdoutLimitProperty,
        new TMemoryGuaranteeProperty,
        new TMemoryLimitProperty,
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
        new TAllowedDevicesProperty,
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
