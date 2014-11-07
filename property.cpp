#include "subsystem.hpp"
#include "property.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/pwd.hpp"

bool TPropertySet::ParentDefault(std::shared_ptr<TContainer> &c,
                                    const std::string &property) {
    TError error = GetSharedContainer(c);
    if (error) {
        TLogger::LogError(error, "Can't get default for " + property);
        return "";
    }

    return c->UseParentNamespace() && HasFlags(property, PARENT_DEF_PROPERTY);
}

bool TPropertySet::IsDefault(const std::string &property) {
    return VariantSet.IsDefault(property);
}

bool TPropertySet::HasFlags(const std::string &property, int flags) {
    // TODO: Log error
    if (!propertySet.Valid(property))
        return false;

    return propertySet.Get(property)->Flags & flags;
}
bool TPropertySet::HasState(const std::string &property, EContainerState state) {
    // TODO: Log error
    if (!propertySet.Valid(property))
        return false;
    auto p = propertySet.Get(property);

    return p->State.find(state) != p->State.end();
}

bool TPropertySet::IsRoot() {
    return Name == ROOT_CONTAINER;
}

TError TPropertySet::Create() {
    kv::TNode node;
    return Storage.SaveNode(Name, node);
}

TError TPropertySet::Restore(const kv::TNode &node) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        TError error = SetString(key, value);
        if (error)
            return error;
    }

    return SyncStorage();
}

TError TPropertySet::Valid(const std::string &property) {
    if (!propertySet.Valid(property))
        return TError(EError::InvalidProperty, "invalid property");
    return TError::Success();
}

TPropertySet::~TPropertySet() {
    if (!IsRoot()) {
        TError error = Storage.RemoveNode(Name);
        TLogger::LogError(error, "Can't remove key-value node " + Name);
    }
}

TError TPropertySet::SyncStorage() {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    for (auto &name : VariantSet.List()) {
        auto pair = node.add_pairs();
        pair->set_key(name);
        pair->set_val(VariantSet.GetString(name));
    }

    return Storage.SaveNode(Name, node);
}

TError TPropertySet::GetSharedContainer(std::shared_ptr<TContainer> &c) {
    c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    return TError::Success();
}

TError TPropertySet::AppendStorage(const std::string& key, const std::string& value) {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return Storage.AppendNode(Name, node);
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
    TLogger::LogError(error, "Can't get cwd for std file");
    if (error)
        return "";

    error = c->GetProperty("root", root);
    TLogger::LogError(error, "Can't get root for std file");
    if (error)
        return "";

    std::string prefix;
    if (c->UseParentNamespace())
        prefix = c->GetName(false) + ".";

    TPath path = root;
    path.AddComponent(cwd);
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
                     0,
                     staticProperty) {}
};

class TUserProperty : public TStringValue {
public:
    TUserProperty() :
        TStringValue(P_USER,
                     "Start command with given user",
                     SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        int uid, gid;
        c->GetPerm(uid, gid);
        TUser u(uid);
        if (u.Load())
            return std::to_string(uid);
        else
            return u.GetName();
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        TUser u(value);
        TError error = u.Load();
        if (error)
            return error;
        return v->Set(EValueType::String, value);
    }
};

class TGroupProperty : public TStringValue {
public:
    TGroupProperty() :
        TStringValue(P_GROUP,
                     "Start command with given group",
                     SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        int uid, gid;
        c->GetPerm(uid, gid);
        TGroup g(gid);
        if (g.Load())
            return std::to_string(gid);
        else
            return g.GetName();
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        TGroup g(value);
        TError error = g.Load();
        if (error)
            return error;
        return v->Set(EValueType::String, value);
    }
};

class TEnvProperty : public TListValue {
public:
    TEnvProperty() :
        TListValue(P_ENV,
                   "Container environment variables",
                   PARENT_DEF_PROPERTY,
                   staticProperty) {}
};

class TRootProperty : public TStringValue {
public:
    TRootProperty() :
        TStringValue(P_ROOT,
                     "Container root directory",
                     PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "/";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        TError error = ValidPath(c, value);
        if (error)
            return error;
        return v->Set(EValueType::String, value);
    }
};

class TCwdProperty : public TStringValue {
public:
    TCwdProperty() :
        TStringValue(P_CWD,
                     "Container working directory",
                     PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        if (!c->Prop->IsDefault("root"))
            return "/";

        return config().container().tmp_dir() + "/" + c->GetName();
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        TError error = ValidPath(c, value);
        if (error)
            return error;
        return v->Set(EValueType::String, value);
    }
};

class TStdinPathProperty : public TStringValue {
public:
    TStdinPathProperty() :
        TStringValue(P_STDIN_PATH,
                     "Container standard input path",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "/dev/null";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        TError error = ExistingFile(c, value);
        if (error)
            return error;
        return v->Set(EValueType::String, value);
    }
};

class TStdoutPathProperty : public TStringValue {
public:
    TStdoutPathProperty() :
        TStringValue(P_STDOUT_PATH,
                     "Container standard input path",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return DefaultStdFile(c, "stdout");
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        TError error = ValidPath(c, value);
        if (error)
            return error;
        return v->Set(EValueType::String, value);
    }
};

class TStderrPathProperty : public TStringValue {
public:
    TStderrPathProperty() :
        TStringValue(P_STDERR_PATH,
                     "Container standard error path",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return DefaultStdFile(c, "stderr");
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        TError error = ValidPath(c, value);
        if (error)
            return error;
        return v->Set(EValueType::String, value);
    }
};

class TStdoutLimitProperty : public TUintValue {
public:
    TStdoutLimitProperty() :
        TUintValue(P_STDOUT_LIMIT,
                   "Return no more than given number of bytes from standard output/error",
                   0,
                   staticProperty) {}

    uint64_t GetDefaultUint(std::shared_ptr<TContainer> c) override {
        return config().container().stdout_limit();
    }

    TError SetUint(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TVariant> v,
                   const uint64_t &value) override {
        uint32_t max = config().container().stdout_limit();

        if (value > max)
            return TError(EError::InvalidValue,
                          "Maximum number of bytes: " +
                          std::to_string(max));

        return v->Set(EValueType::Uint, value);
    }
};

class TMemoryGuaranteeProperty : public TUintValue {
public:
    TMemoryGuaranteeProperty() :
        TUintValue(P_MEM_GUARANTEE,
                   "Guaranteed amount of memory",
                   PARENT_RO_PROPERTY,
                   dynamicProperty) {}

    TError SetUint(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TVariant> v,
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

        return v->Set(EValueType::Uint, value);
    }
};

class TMemoryLimitProperty : public TUintValue {
public:
    TMemoryLimitProperty() :
        TUintValue(P_MEM_LIMIT,
                   "Memory hard limit",
                   0,
                   dynamicProperty) {}

    TError SetUint(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TVariant> v,
                   const uint64_t &value) override {
        if (!c->ValidHierarchicalProperty(P_MEM_LIMIT, value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        return v->Set(EValueType::Uint, value);
    }
};

class TRechargeOnPgfaultProperty : public TBoolValue {
public:
    TRechargeOnPgfaultProperty() :
        TBoolValue(P_RECHARGE_ON_PGFAULT,
                   "Recharge memory on page fault",
                   PARENT_RO_PROPERTY,
                   dynamicProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) override {
        return false;
    }

    TError SetBool(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TVariant> v,
                   const bool &value) override {
        auto memroot = memorySubsystem->GetRootCgroup();
        if (!memroot->HasKnob("memory.recharge_on_pgfault"))
            return TError(EError::NotSupported, "invalid kernel");

        return v->Set(EValueType::Bool, value);
    }
};

class TCpuPolicyProperty : public TStringValue {
public:
    TCpuPolicyProperty() :
        TStringValue(P_CPU_POLICY,
                     "CPU policy: rt, normal, idle",
                     PARENT_RO_PROPERTY,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "normal";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
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

        return v->Set(EValueType::String, value);
    }
};

class TCpuPriorityProperty : public TUintValue {
public:
    TCpuPriorityProperty() :
        TUintValue(P_CPU_PRIO,
                   "CPU priority: 0-99",
                   PARENT_RO_PROPERTY,
                   dynamicProperty) {}

    uint64_t GetDefaultUint(std::shared_ptr<TContainer> c) override {
        return DEF_CLASS_PRIO;
    }

    TError SetUint(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TVariant> v,
                   const uint64_t &value) override {
        if (value < 0 || value > 99)
            return TError(EError::InvalidValue, "invalid value");

        return v->Set(EValueType::Uint, value);
    }
};

class TNetMapValue : public TMapValue {
    uint64_t Def;
protected:
    TError CheckNetMap(const TUintMap &m,
                       std::shared_ptr<TContainer> c) {
        std::set<std::string> validKey;

        for (auto &link : c->Links)
            validKey.insert(link->GetName());

        for (auto &kv : m)
            if (validKey.find(kv.first) == validKey.end())
                return TError(EError::InvalidValue,
                              "invalid interface " + kv.first);

        return TError::Success();
    }

public:
    TNetMapValue(const std::string &name,
                 const std::string &desc,
                 const uint64_t def) :
        TMapValue(name, desc,
                  PARENT_RO_PROPERTY,
                  staticProperty), Def(def) {}

    TUintMap GetDefaultMap(std::shared_ptr<TContainer> c) override {
        TUintMap m;
        for (auto &link : c->Links)
            m[link->GetName()] = Def;
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
                     "Guaranteed container network bandwidth",
                     DEF_CLASS_RATE) {}
};

class TNetCeilProperty : public TNetMapValue {
public:
    TNetCeilProperty() :
        TNetMapValue(P_NET_CEIL,
                     "Maximum container network bandwidth",
                     DEF_CLASS_CEIL) {}
};

class TNetPriorityProperty : public TNetMapValue {
public:
    TNetPriorityProperty() :
        TNetMapValue(P_NET_PRIO,
                  "Container network priority: 0-7",
                  DEF_CLASS_NET_PRIO) {}

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
                   0,
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
                  0,
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
                   0,
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
                     0,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) override {
        return "";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::string &value) override {
        uint32_t max = config().container().private_max();

        if (value.length() > max)
            return TError(EError::InvalidValue, "Value is too long");

        return v->Set(EValueType::String, value);
    }
};

class TUlimitProperty : public TListValue {
public:
    TUlimitProperty() :
        TListValue(P_ULIMIT,
                   "Container resource limits",
                   PARENT_DEF_PROPERTY,
                   staticProperty) {}

    TError SetList(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const std::vector<std::string> &value) override {
        std::map<int, struct rlimit> rlim;
        TError error = ParseRlimit(value, rlim);
        if (error)
            return error;
        return v->Set(EValueType::List, value);
    }
};

class THostnameProperty : public TStringValue {
public:
    THostnameProperty() :
        TStringValue(P_HOSTNAME,
                     "Container hostname",
                     0,
                     staticProperty) {}
};

class TBindDnsProperty : public TBoolValue {
public:
    TBindDnsProperty() :
        TBoolValue(P_BIND_DNS,
                   "Bind /etc/resolv.conf and /etc/hosts of host to container",
                   0,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) override {
        if (c->Prop->IsDefault("root"))
            return false;
        else
            return true;
    }
};

class TBindProperty : public TListValue {
public:
    TBindProperty() :
        TListValue(P_BIND,
                   "Share host directories with container",
                   0,
                   staticProperty) {}

    TError SetList(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const TStrList &value) override {
        TLogger::Log() << "SET BIND TO " << ListToString(value) << std::endl;
        std::vector<TBindMap> dirs;
        TError error = ParseBind(value, dirs);
        if (error)
            return error;
        return v->Set(EValueType::List, value);
    }
};

class TNetProperty : public TListValue {
public:
    TNetProperty() :
        TListValue(P_NET,
                   "Container network settings",
                   0,
                   staticProperty) {}

    TStrList GetDefaultList(std::shared_ptr<TContainer> c) override {
        return TStrList{ "host" };
    }

    TError SetList(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v,
                     const TStrList &value) override {
        TNetCfg net;
        TError error = ParseNet(c, value, net);
        if (error)
            return error;
        return v->Set(EValueType::List, value);
    }
};

class TAllowedDevicesProperty : public TListValue {
public:
    TAllowedDevicesProperty() :
        TListValue(P_ALLOWED_DEVICES,
                   "Devices that container can create/read/write",
                   0,
                   staticProperty) {}

    TStrList GetDefaultList(std::shared_ptr<TContainer> c) override {
        return TStrList{ "a *:* rwm" };
    }
};

class TUidProperty : public TIntValue {
public:
    TUidProperty() : TIntValue(P_RAW_UID, "", HIDDEN_VALUE, {}) {}
};

class TGidProperty : public TIntValue {
public:
    TGidProperty() : TIntValue(P_RAW_GID, "", HIDDEN_VALUE, {}) {}
};

class TIdProperty : public TIntValue {
public:
    TIdProperty() : TIntValue(P_RAW_ID, "", HIDDEN_VALUE, {}) {}
};

class TRootPidProperty : public TIntValue {
public:
    TRootPidProperty() : TIntValue(P_RAW_ROOT_PID, "", HIDDEN_VALUE, {}) {}
};

TValueSet propertySet;
TError RegisterProperties() {
    std::vector<TValue *> properties = {
        new TCommandProperty,
        new TUserProperty,
        new TGroupProperty,
        new TEnvProperty,
        new TRootProperty,
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

        new TUidProperty,
        new TGidProperty,
        new TIdProperty,
        new TRootPidProperty,
    };

    return propertySet.Register(properties);
}

TError ParseRlimit(const std::vector<std::string> &limits, std::map<int,struct rlimit> &rlim) {
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

    for (auto &limit : limits) {
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

        rlim[idx].rlim_cur = soft;
        rlim[idx].rlim_max = hard;
    }

    return TError::Success();
}

TError ParseBind(const std::vector<std::string> &lines, std::vector<TBindMap> &dirs) {
    for (auto &line : lines) {
        std::vector<std::string> tok;
        TBindMap bindMap;

        TError error = SplitEscapedString(line, ' ', tok);
        if (error)
            return error;

        if (tok.size() != 2 && tok.size() != 3)
            return TError(EError::InvalidValue, "Invalid bind in: " + line);

        bindMap.Source = tok[0];
        bindMap.Dest = tok[1];
        bindMap.Rdonly = false;

        if (tok.size() == 3) {
            if (tok[2] == "ro")
                bindMap.Rdonly = true;
            else if (tok[2] == "rw")
                bindMap.Rdonly = false;
            else
                return TError(EError::InvalidValue, "Invalid bind type in: " + line);
        }

        if (!bindMap.Source.Exists())
            return TError(EError::InvalidValue, "Source bind " + bindMap.Source.ToString() + " doesn't exist");

        dirs.push_back(bindMap);
    }

    return TError::Success();
}

TError ParseNet(std::shared_ptr<const TContainer> container, const std::vector<std::string> &lines, TNetCfg &net) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

    bool none = false;
    net.Share = false;

    if (lines.size() == 0)
        return TError(EError::InvalidValue, "Configuration is not specified");

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

        if (net.Share)
            return TError(EError::InvalidValue,
                          "host can't be mixed with other settings");

        if (type == "none") {
            none = true;
        } else if (type == "host") {
            THostNetCfg hnet;

            if (settings.size() > 2)
                return TError(EError::InvalidValue, "Invalid net in: " + line);

            if (settings.size() == 1) {
                net.Share = true;
            } else {
                hnet.Dev = StringTrim(settings[1]);

                auto link = container->ValidLink(hnet.Dev);
                if (!link)
                    return TError(EError::InvalidValue,
                                  "Invalid host interface " + hnet.Dev);

                net.Host.push_back(hnet);
            }
        } else if (type == "macvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid macvlan in: " + line);

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string type = "bridge";
            std::string hw = "";

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
                hw = StringTrim(settings[4]);
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

            net.MacVlan.push_back(mvlan);
        } else {
            return TError(EError::InvalidValue, "Configuration is not specified");
        }
    }

    return TError::Success();
}
