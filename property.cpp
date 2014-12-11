#include "property.hpp"
#include "config.hpp"
#include "subsystem.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "qdisc.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/pwd.hpp"
#include "util/netlink.hpp"

extern "C" {
#include <linux/capability.h>
}

bool TPropertySet::ParentDefault(std::shared_ptr<TContainer> &c,
                                 const std::string &property) {
    TError error = GetSharedContainer(c);
    if (error) {
        L_ERR() << "Can't get default for " << property << ": " << error << std::endl;
        return "";
    }

    return c->UseParentNamespace() && HasFlags(property, PARENT_DEF_PROPERTY);
}

bool TPropertySet::IsDefault(const std::string &property) {
    return VariantSet.IsDefault(property);
}

bool TPropertySet::HasFlags(const std::string &property, int flags) {
    if (!propertySet.Valid(property)) {
        TError error(EError::Unknown, "Invalid property " + property);
        L_ERR() << "Invalid property: " << error << std::endl;
        return false;
    }

    return propertySet.Get(property)->Flags & flags;
}
bool TPropertySet::HasState(const std::string &property, EContainerState state) {
    if (!propertySet.Valid(property)) {
        TError error(EError::Unknown, "Invalid property " + property);
        L_ERR() << "Can't test property state: " << error << std::endl;
        return false;
    }
    auto p = propertySet.Get(property);

    return p->State.find(state) != p->State.end();
}

TError TPropertySet::Valid(const std::string &property) {
    if (!propertySet.Valid(property))
        return TError(EError::InvalidProperty, "invalid property");
    return TError::Success();
}

TError TPropertySet::Create() {
    return VariantSet.Create();
}

TError TPropertySet::Restore(const kv::TNode &node) {
    return VariantSet.Restore(node);
}

bool TPropertySet::HasValue(const std::string &name) {
    return VariantSet.HasValue(name);
}

TError TPropertySet::Flush() {
    return VariantSet.Flush();
}

TError TPropertySet::Sync() {
    return VariantSet.Sync();
}

TError TPropertySet::PrepareTaskEnv(const std::string &property,
                                    std::shared_ptr<TTaskEnv> taskEnv) {
    std::shared_ptr<TContainer> c;
    TValue *p;
    std::shared_ptr<TVariant> v;

    TError error = VariantSet.Get(property, c, &p, v);
    if (error)
        return error;

    if (IsDefault(property)) {
        TError error = p->ParseDefault(c);
        if (error)
            return error;
    }

    return p->PrepareTaskEnv(c, taskEnv);
}

TError TPropertySet::GetSharedContainer(std::shared_ptr<TContainer> &c) {
    c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    return TError::Success();
}

static TError ValidPath(std::shared_ptr<TContainer> c, const std::string &str) {
    if (!str.length() || str[0] != '/')
        return TError(EError::InvalidValue, "invalid directory");
    return TError::Success();
}

static TError ExistingFile(std::shared_ptr<TContainer> c, const std::string &str) {
    TFile f(str);
    if (!f.Exists())
        return TError(EError::InvalidValue, "file doesn't exist");
    return TError::Success();
}

static std::string DefaultStdFile(std::shared_ptr<TContainer> c,
                                  const std::string &name) {
    std::string cwd, root;
    TError error = c->GetProperty("cwd", cwd);
    if (error) {
        L_ERR() << "Can't get cwd for std file: " << error << std::endl;
        return "";
    }

    error = c->GetProperty("root", root);
    if (error) {
        L_ERR() << "Can't get root for std file: " << error << std::endl;
        return "";
    }

    std::string prefix;
    if (c->UseParentNamespace())
        prefix = c->GetName(false) + ".";

    TPath path = root;
    if (!path.Exists() || path.GetType() == EFileType::Directory) {
        path.AddComponent(cwd);
    } else {
        path = c->GetTmpDir();
    }

    path.AddComponent(prefix + name);
    return path.ToString();
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

class TCommandProperty : public TStringValue {
public:
    TCommandProperty() :
        TStringValue(P_COMMAND,
                     "Command executed upon container start",
                     PERSISTENT_VALUE,
                     staticProperty) {}
};

class TUserProperty : public TStringValue {
public:
    TUserProperty() :
        TStringValue(P_USER,
                     "Start command with given user",
                     NODEF_VALUE | SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY | PERSISTENT_VALUE,
                     staticProperty) {}

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        TUser u(value);
        TError error = u.Load();
        if (error)
            return error;

        c->Uid = u.GetId();

        return TError::Success();
    }
};

class TGroupProperty : public TStringValue {
public:
    TGroupProperty() :
        TStringValue(P_GROUP,
                     "Start command with given group",
                     NODEF_VALUE | SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY | PERSISTENT_VALUE,
                     staticProperty) {}

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        TGroup g(value);
        TError error = g.Load();
        if (error)
            return error;

        c->Gid = g.GetId();

        return TError::Success();
    }
};

class TEnvProperty : public TListValue {
public:
    TEnvProperty() :
        TListValue(P_ENV,
                   "Container environment variables",
                   PARENT_DEF_PROPERTY | PERSISTENT_VALUE,
                   staticProperty) {}
};

class TRootProperty : public TStringValue {
public:
    TRootProperty() :
        TStringValue(P_ROOT,
                     "Container root directory",
                     PARENT_DEF_PROPERTY | PERSISTENT_VALUE,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "/";
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        return ValidPath(c, value);
    }
};

class TRootRdOnlyProperty : public TBoolValue {
public:
    TRootRdOnlyProperty() :
        TBoolValue(P_ROOT_RDONLY,
                   "Mount root directory in read-only mode",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) override {
        return false;
    }
};

class TCwdProperty : public TStringValue {
public:
    TCwdProperty() :
        TStringValue(P_CWD,
                     "Container working directory",
                     PARENT_DEF_PROPERTY | PERSISTENT_VALUE,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        if (!c->Prop->IsDefault("root"))
            return "/";

        return config().container().tmp_dir() + "/" + c->GetName();
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        return ValidPath(c, value);
    }
};

class TStdinPathProperty : public TStringValue {
public:
    TStdinPathProperty() :
        TStringValue(P_STDIN_PATH,
                     "Container standard input path",
                     PERSISTENT_VALUE,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "/dev/null";
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        return ExistingFile(c, value);
    }
};

class TStdoutPathProperty : public TStringValue {
public:
    TStdoutPathProperty() :
        TStringValue(P_STDOUT_PATH,
                     "Container standard input path",
                     PERSISTENT_VALUE,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return DefaultStdFile(c, "stdout");
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        return ValidPath(c, value);
    }
};

class TStderrPathProperty : public TStringValue {
public:
    TStderrPathProperty() :
        TStringValue(P_STDERR_PATH,
                     "Container standard error path",
                     PERSISTENT_VALUE,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return DefaultStdFile(c, "stderr");
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        return ValidPath(c, value);
    }
};

class TStdoutLimitProperty : public TUintValue {
public:
    TStdoutLimitProperty() :
        TUintValue(P_STDOUT_LIMIT,
                   "Return no more than given number of bytes from standard output/error",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    uint64_t GetDefaultUint(std::shared_ptr<TContainer> c) override {
        return config().container().stdout_limit();
    }

    TError ParseUint(std::shared_ptr<TContainer> c,
                     const uint64_t &value) override {
        uint32_t max = config().container().stdout_limit();

        if (value > max)
            return TError(EError::InvalidValue,
                          "Maximum number of bytes: " +
                          std::to_string(max));

        return TError::Success();
    }
};

class TMemoryGuaranteeProperty : public TUintValue {
public:
    TMemoryGuaranteeProperty() :
        TUintValue(P_MEM_GUARANTEE,
                   "Guaranteed amount of memory",
                   PARENT_RO_PROPERTY | PERSISTENT_VALUE,
                   dynamicProperty) {}

    TError ParseUint(std::shared_ptr<TContainer> c,
                     const uint64_t &value) override {
        auto memroot = memorySubsystem->GetRootCgroup();
        if (!memroot->HasKnob("memory.low_limit_in_bytes"))
            return TError(EError::NotSupported, "invalid kernel");

        if (!c->ValidHierarchicalProperty(P_MEM_GUARANTEE, value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        uint64_t total = c->GetRoot()->GetChildrenSum(P_MEM_GUARANTEE, c, value);
        if (total + config().daemon().memory_guarantee_reserve() >
            GetTotalMemory())
            return TError(EError::ResourceNotAvailable,
                          "can't guarantee all available memory");

        return TError::Success();
    }
};

class TMemoryLimitProperty : public TUintValue {
public:
    TMemoryLimitProperty() :
        TUintValue(P_MEM_LIMIT,
                   "Memory hard limit",
                   PERSISTENT_VALUE,
                   dynamicProperty) {}

    TError ParseUint(std::shared_ptr<TContainer> c,
                     const uint64_t &value) override {
        if (!c->ValidHierarchicalProperty(P_MEM_LIMIT, value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        return TError::Success();
    }
};

class TRechargeOnPgfaultProperty : public TBoolValue {
public:
    TRechargeOnPgfaultProperty() :
        TBoolValue(P_RECHARGE_ON_PGFAULT,
                   "Recharge memory on page fault",
                   PARENT_RO_PROPERTY | PERSISTENT_VALUE,
                   dynamicProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) override {
        return false;
    }

    TError ParseBool(std::shared_ptr<TContainer> c,
                     const bool &value) override {
        auto memroot = memorySubsystem->GetRootCgroup();
        if (!memroot->HasKnob("memory.recharge_on_pgfault"))
            return TError(EError::NotSupported, "invalid kernel");

        return TError::Success();
    }
};

class TCpuPolicyProperty : public TStringValue {
public:
    TCpuPolicyProperty() :
        TStringValue(P_CPU_POLICY,
                     "CPU policy: rt, normal, idle",
                     PARENT_RO_PROPERTY | PERSISTENT_VALUE,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "normal";
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        if (value != "normal" && value != "rt" && value != "idle")
            return TError(EError::InvalidValue, "invalid policy");

        if (value == "rt") {
            auto cpuroot = cpuSubsystem->GetRootCgroup();
            if (!cpuroot->HasKnob("cpu.smart"))
                return TError(EError::NotSupported, "invalid kernel");
        }

        if (value == "idle")
            return TError(EError::NotSupported, "not implemented");

        return TError::Success();
    }
};

class TCpuPriorityProperty : public TUintValue {
public:
    TCpuPriorityProperty() :
        TUintValue(P_CPU_PRIO,
                   "CPU priority: 0-99",
                   PARENT_RO_PROPERTY | PERSISTENT_VALUE,
                   dynamicProperty) {}

    uint64_t GetDefaultUint(std::shared_ptr<TContainer> c) override {
        return config().container().default_cpu_prio();
    }

    TError ParseUint(std::shared_ptr<TContainer> c,
                     const uint64_t &value) override {
        if (value < 0 || value > 99)
            return TError(EError::InvalidValue, "invalid value");

        return TError::Success();
    }
};

class TNetMapValue : public TMapValue {
    uint64_t Def, RootDef;
protected:
    TError CheckNetMap(const TUintMap &m,
                       std::shared_ptr<TContainer> c) {
        std::set<std::string> validKey;

        for (auto &link : c->Net->GetLinks())
            validKey.insert(link->GetAlias());

        for (auto &kv : m)
            if (validKey.find(kv.first) == validKey.end())
                return TError(EError::InvalidValue,
                              "invalid interface " + kv.first);

        return TError::Success();
    }

public:
    TNetMapValue(const std::string &name,
                 const std::string &desc) :
        TMapValue(name, desc,
                  PARENT_RO_PROPERTY,
                  staticProperty) {}

    virtual uint32_t GetDefault() { return 0; }
    virtual uint32_t GetRootDefault() { return 0; }

    TUintMap GetDefaultMap(std::shared_ptr<TContainer> c) override {
        uint64_t def =  c->IsRoot() ? GetRootDefault() : GetDefault();

        TUintMap m;
        for (auto &link : c->Net->GetLinks())
            m[link->GetAlias()] = def;
        return m;
    }

    TError SetMap(std::shared_ptr<TContainer> c,
                  std::shared_ptr<TVariant> v,
                  const TUintMap &value) override {
        TError error = CheckNetMap(value, c);
        if (error)
            return error;

        TUintMap m = GetMap(c, v);
        for (auto &kv : value)
            m[kv.first] = kv.second;

        return TMapValue::SetMap(c, v, m);
    }
};

class TNetGuaranteeProperty : public TNetMapValue {
public:
    TNetGuaranteeProperty() :
        TNetMapValue(P_NET_GUARANTEE,
                     "Guaranteed container network bandwidth [bytes/s] (max 32Gbps)") {}
    uint32_t GetDefault() override { return config().network().default_guarantee(); }
    uint32_t GetRootDefault() override { return config().network().default_max_guarantee(); }
};

class TNetCeilProperty : public TNetMapValue {
public:
    TNetCeilProperty() :
        TNetMapValue(P_NET_CEIL,
                     "Maximum container network bandwidth [bytes/s] (max 32Gbps)") {}
    uint32_t GetDefault() override { return config().network().default_limit(); }
    uint32_t GetRootDefault() override { return config().network().default_max_guarantee(); }
};

class TNetPriorityProperty : public TNetMapValue {
public:
    TNetPriorityProperty() :
        TNetMapValue(P_NET_PRIO,
                     "Container network priority: 0-7") {}
    uint32_t GetDefault() override { return config().network().default_prio(); }
    uint32_t GetRootDefault() override { return config().network().default_prio(); }

    TError SetMap(std::shared_ptr<TContainer> c,
                  std::shared_ptr<TVariant> v,
                  const TUintMap &value) override {

        TError error = CheckNetMap(value, c);
        if (error)
            return error;

        TUintMap m = GetMap(c, v);
        for (auto &kv : value) {
            m[kv.first] = kv.second;

            if (kv.second > 7)
                return TError(EError::InvalidValue, "invalid value");
        }

        return TMapValue::SetMap(c, v, m);
    }
};

class TRespawnProperty : public TBoolValue {
public:
    TRespawnProperty() :
        TBoolValue(P_RESPAWN,
                   "Automatically respawn dead container",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) override {
        return false;
    }
};

class TMaxRespawnsProperty : public TIntValue {
public:
    TMaxRespawnsProperty() :
        TIntValue(P_MAX_RESPAWNS,
                  "Limit respawn count for specific container",
                  PERSISTENT_VALUE,
                  staticProperty) {}

    int GetDefaultInt(std::shared_ptr<TContainer> c) override {
        return -1;
    }
};

class TIsolateProperty : public TBoolValue {
public:
    TIsolateProperty() :
        TBoolValue(P_ISOLATE,
                   "Isolate container from parent",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) override {
        return true;
    }
};

class TPrivateProperty : public TStringValue {
public:
    TPrivateProperty() :
        TStringValue(P_PRIVATE,
                     "User-defined property",
                     PERSISTENT_VALUE,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "";
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        uint32_t max = config().container().private_max();

        if (value.length() > max)
            return TError(EError::InvalidValue, "Value is too long");

        return TError::Success();
    }
};

class TUlimitProperty : public TListValue {
    std::map<int,struct rlimit> Rlimit;

public:
    TUlimitProperty() :
        TListValue(P_ULIMIT,
                   "Container resource limits",
                   PARENT_DEF_PROPERTY | PERSISTENT_VALUE,
                   staticProperty) {}

    TError ParseList(std::shared_ptr<TContainer> container,
                     const std::vector<std::string> &lines) override {
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
            if (softhard[0] == "unlim" || softhard[0] == "unliminted") {
                soft = RLIM_INFINITY;
            } else {
                TError error = StringToUint64(softhard[0], soft);
                if (error)
                    return TError(EError::InvalidValue, "Invalid soft limit for " + name);
            }

            if (softhard[1] == "unlim" || softhard[1] == "unliminted") {
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

    TError PrepareTaskEnv(std::shared_ptr<TContainer> container,
                          std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->Rlimit = Rlimit;
        return TError::Success();
    }
};

class THostnameProperty : public TStringValue {
public:
    THostnameProperty() :
        TStringValue(P_HOSTNAME,
                     "Container hostname",
                     PERSISTENT_VALUE,
                     staticProperty) {}
};

class TBindDnsProperty : public TBoolValue {
public:
    TBindDnsProperty() :
        TBoolValue(P_BIND_DNS,
                   "Bind /etc/resolv.conf and /etc/hosts of host to container",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) override {
        if (c->Prop->IsDefault("root"))
            return false;
        else
            return true;
    }
};

class TBindProperty : public TListValue {
    std::vector<TBindMap> BindMap;

public:
    TBindProperty() :
        TListValue(P_BIND,
                   "Share host directories with container",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    TError ParseList(std::shared_ptr<TContainer> container,
                     const std::vector<std::string> &lines) override {
        BindMap.clear();

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

            if (!m.Source.Exists())
                return TError(EError::InvalidValue, "Source bind " + m.Source.ToString() + " doesn't exist");

            BindMap.push_back(m);
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TContainer> container,
                          std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->BindMap = BindMap;
        return TError::Success();
    }
};

class TDefaultGwProperty : public TStringValue {
    struct TNlAddr Addr;
public:
    TDefaultGwProperty() :
        TStringValue(P_DEFAULT_GW,
                     "Default gateway",
                     PERSISTENT_VALUE | HIDDEN_VALUE,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "0.0.0.0";
    }

    TError ParseString(std::shared_ptr<TContainer> c,
                       const std::string &value) override {
        return Addr.Parse(value);
    }

    TError PrepareTaskEnv(std::shared_ptr<TContainer> container,
                          std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->DefaultGw = Addr;
        return TError::Success();
    }
};

class TIpProperty : public TListValue {
    std::map<std::string, TIpMap> IpMap;

public:
    TIpProperty() :
        TListValue(P_IP,
                   "IP configuration",
                   PERSISTENT_VALUE | HIDDEN_VALUE,
                   staticProperty) {}

    TStrList GetDefaultList(std::shared_ptr<TContainer> c) override {
        return TStrList{ "- 0.0.0.0/0" };
    }

    TError ParseList(std::shared_ptr<TContainer> container,
                     const std::vector<std::string> &lines) override {
        IpMap.clear();
        for (auto &line : lines) {
            std::vector<std::string> settings;
            TError error = SplitEscapedString(line, ' ', settings);
            if (error)
                return error;

            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid address/prefix in: " + line);

            TIpMap ip;
            error = ParseIpPrefix(settings[1], ip.Addr, ip.Prefix);
            if (error)
                return error;

            IpMap[settings[0]] = ip;
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TContainer> container,
                          std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->IpMap = IpMap;
        return TError::Success();
    }
};

class TNetProperty : public TListValue {
    TNetCfg NetCfg;

public:
    TNetProperty() :
        TListValue(P_NET,
                   "Container network settings",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    TStrList GetDefaultList(std::shared_ptr<TContainer> c) override {
        return TStrList{ "host" };
    }

    TError ParseList(std::shared_ptr<TContainer> container,
                     const std::vector<std::string> &lines) override {
        if (!config().network().enabled())
            return TError(EError::Unknown, "Network support is disabled");

        bool none = false;
        NetCfg.Share = false;
        NetCfg.Host.clear();
        NetCfg.MacVlan.clear();
        NetCfg.Veth.clear();
        int idx = 0;

        if (lines.size() == 0)
            return TError(EError::InvalidValue, "Configuration is not specified");

        /*
        TError error = container->UpdateLinkCache();
        if (error)
            return error;
            */

        for (auto &line : lines) {
            if (none)
                return TError(EError::InvalidValue,
                              "none can't be mixed with other types");

            std::vector<std::string> settings;

            TError error = SplitEscapedString(line, ' ', settings);
            if (error)
                return error;

            if (settings.size() == 0)
                return TError(EError::InvalidValue, "Invalid net in: " + line);

            std::string type = StringTrim(settings[0]);

            if (NetCfg.Share)
                return TError(EError::InvalidValue,
                              "host can't be mixed with other settings");

            if (type == "none") {
                none = true;
            } else if (type == "host") {
                THostNetCfg hnet;

                if (settings.size() > 2)
                    return TError(EError::InvalidValue, "Invalid net in: " + line);

                if (settings.size() == 1) {
                    NetCfg.Share = true;
                } else {
                    hnet.Dev = StringTrim(settings[1]);

                    auto link = container->ValidLink(hnet.Dev);
                    if (!link)
                        return TError(EError::InvalidValue,
                                      "Invalid host interface " + hnet.Dev);

                    NetCfg.Host.push_back(hnet);
                }
            } else if (type == "macvlan") {
                // macvlan <master> <name> [type] [mtu] [hw]

                if (settings.size() < 3)
                    return TError(EError::InvalidValue, "Invalid macvlan in: " + line);

                std::string master = StringTrim(settings[1]);
                std::string name = StringTrim(settings[2]);
                std::string type = "bridge";
                std::string hw = "";
                int mtu = -1;

                auto link = container->GetLink(master);
                if (!link)
                    return TError(EError::InvalidValue,
                                  "Invalid macvlan master " + master);

                if (settings.size() > 3) {
                    type = StringTrim(settings[3]);
                    if (!TNlLink::ValidMacVlanType(type))
                        return TError(EError::InvalidValue,
                                      "Invalid macvlan type " + type);
                }

                if (settings.size() > 4) {
                    TError error = StringToInt(settings[4], mtu);
                    if (error)
                        return TError(EError::InvalidValue,
                                      "Invalid macvlan mtu " + settings[4]);
                }

                if (settings.size() > 5) {
                    hw = StringTrim(settings[5]);
                    if (!TNlLink::ValidMacAddr(hw))
                        return TError(EError::InvalidValue,
                                      "Invalid macvlan address " + hw);
                }

                int idx = link->FindIndex(master);
                if (idx < 0)
                    return TError(EError::InvalidValue, "Interface " + master + " doesn't exist or not in running state");

                TMacVlanNetCfg mvlan;
                mvlan.Master = master;
                mvlan.Name = name;
                mvlan.Type = type;
                mvlan.Hw = hw;
                mvlan.Mtu = mtu;

                NetCfg.MacVlan.push_back(mvlan);
            } else if (type == "veth") {
                // veth <name> <bridge> [mtu] [hw]

                if (settings.size() < 3)
                    return TError(EError::InvalidValue, "Invalid veth in: " + line);
                std::string name = StringTrim(settings[1]);
                std::string bridge = StringTrim(settings[2]);
                std::string hw = "";
                int mtu = -1;

                if (settings.size() > 3) {
                    TError error = StringToInt(settings[3], mtu);
                    if (error)
                        return TError(EError::InvalidValue,
                                      "Invalid veth mtu " + settings[3]);
                }

                if (settings.size() > 4) {
                    hw = StringTrim(settings[4]);
                    if (!TNlLink::ValidMacAddr(hw))
                        return TError(EError::InvalidValue,
                                      "Invalid veth address " + hw);
                }

                if (!container->ValidLink(bridge))
                    return TError(EError::InvalidValue, "Interface " + bridge + " doesn't exist or not in running state");

                TVethNetCfg veth;
                veth.Bridge = bridge;
                veth.Name = name;
                veth.Hw = hw;
                veth.Mtu = mtu;
                veth.Peer = "portove-" + std::to_string(container->GetId()) + "-" + std::to_string(idx++);

                NetCfg.Veth.push_back(veth);
            } else {
                return TError(EError::InvalidValue, "Configuration is not specified");
            }
        }

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TContainer> container,
                          std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->NetCfg = NetCfg;
        return TError::Success();
    }
};

class TAllowedDevicesProperty : public TListValue {
public:
    TAllowedDevicesProperty() :
        TListValue(P_ALLOWED_DEVICES,
                   "Devices that container can create/read/write",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    TStrList GetDefaultList(std::shared_ptr<TContainer> c) override {
        return TStrList{ "a *:* rwm" };
    }
};

class TCapabilitiesProperty : public TListValue {
    uint64_t Caps;
    const std::map<std::string, uint64_t> supported = {
        { "CHOWN", CAP_CHOWN },
        { "DAC_OVERRIDE", CAP_DAC_OVERRIDE },
        { "DAC_READ_SEARCH", CAP_DAC_READ_SEARCH },
        { "FOWNER", CAP_FOWNER },
        { "FSETID", CAP_FSETID },
        { "KILL", CAP_KILL },
        { "SETGID", CAP_SETGID },
        { "SETUID", CAP_SETUID },
        { "SETPCAP", CAP_SETPCAP },
        { "LINUX_IMMUTABLE", CAP_LINUX_IMMUTABLE },
        { "NET_BIND_SERVICE", CAP_NET_BIND_SERVICE },
        { "NET_BROADCAST", CAP_NET_BROADCAST },
        { "NET_ADMIN", CAP_NET_ADMIN },
        { "NET_RAW", CAP_NET_RAW },
        { "IPC_LOCK", CAP_IPC_LOCK },
        { "IPC_OWNER", CAP_IPC_OWNER },
        { "SYS_MODULE", CAP_SYS_MODULE },
        { "SYS_RAWIO", CAP_SYS_RAWIO },
        { "SYS_CHROOT", CAP_SYS_CHROOT },
        { "SYS_PTRACE", CAP_SYS_PTRACE },
        { "SYS_PACCT", CAP_SYS_PACCT },
        { "SYS_ADMIN", CAP_SYS_ADMIN },
        { "SYS_BOOT", CAP_SYS_BOOT },
        { "SYS_NICE", CAP_SYS_NICE },
        { "SYS_RESOURCE", CAP_SYS_RESOURCE },
        { "SYS_TIME", CAP_SYS_TIME },
        { "SYS_TTY_CONFIG", CAP_SYS_TTY_CONFIG },
        { "MKNOD", CAP_MKNOD },
        { "LEASE", CAP_LEASE },
        { "AUDIT_WRITE", CAP_AUDIT_WRITE },
        { "AUDIT_CONTROL", CAP_AUDIT_CONTROL },
        { "SETFCAP", CAP_SETFCAP },
        { "MAC_OVERRIDE", CAP_MAC_OVERRIDE },
        { "MAC_ADMIN", CAP_MAC_ADMIN },
        { "SYSLOG", CAP_SYSLOG },
        { "WAKE_ALARM", CAP_WAKE_ALARM },
        { "BLOCK_SUSPEND", CAP_BLOCK_SUSPEND },
    };

public:
    TCapabilitiesProperty() :
        TListValue(P_CAPABILITIES,
                   "Limit container capabilities",
                   PERSISTENT_VALUE,
                   staticProperty) {}

    TStrList GetDefaultList(std::shared_ptr<TContainer> c) override {
        TStrList v;
        if (c->Uid == 0 || c->Gid == 0)
            for (auto kv : supported)
                v.push_back(kv.first);
        return v;
    }

    TError SetList(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TVariant> v,
                   const std::vector<std::string> &lines) override {
        if (c->Uid != 0 && c->Gid != 0)
            return TError(EError::Permission, "Permission denied");

        return TListValue::SetList(c, v, lines);
    }

    TError ParseList(std::shared_ptr<TContainer> c,
                     const std::vector<std::string> &lines) override {
        uint64_t allowed = 0;

        for (auto &line: lines) {
            if (supported.find(line) == supported.end())
                return TError(EError::InvalidValue,
                              "Unsupported capability " + line);

            allowed |= (1ULL << supported.at(line));
        }

        Caps = allowed;

        return TError::Success();
    }

    TError PrepareTaskEnv(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TTaskEnv> taskEnv) override {
        taskEnv->Caps = Caps;
        return TError::Success();
    }
};

class TIdProperty : public TIntValue {
public:
    TIdProperty() : TIntValue(P_RAW_ID, "", HIDDEN_VALUE | PERSISTENT_VALUE, {}) {}
};

class TRootPidProperty : public TIntValue {
public:
    TRootPidProperty() : TIntValue(P_RAW_ROOT_PID, "", HIDDEN_VALUE | PERSISTENT_VALUE, {}) {}
};

TValueSet propertySet;
TError RegisterProperties() {
    std::vector<TValue *> properties = {
        new TCommandProperty,
        new TUserProperty,
        new TGroupProperty,
        new TEnvProperty,
        new TRootProperty,
        new TRootRdOnlyProperty,
        new TCwdProperty,
        new TStdinPathProperty,
        new TStdoutPathProperty,
        new TStderrPathProperty,
        new TStdoutLimitProperty,
        new TMemoryGuaranteeProperty,
        new TMemoryLimitProperty,
        new TRechargeOnPgfaultProperty,
        new TCpuPolicyProperty,
        new TCpuPriorityProperty,
        new TNetGuaranteeProperty,
        new TNetCeilProperty,
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
        new TAllowedDevicesProperty,
        new TCapabilitiesProperty,
        new TIpProperty,
        new TDefaultGwProperty,

        new TIdProperty,
        new TRootPidProperty,
    };

    return propertySet.Register(properties);
}
