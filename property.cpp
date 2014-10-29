#include <map>

#include "subsystem.hpp"
#include "property.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/pwd.hpp"

using std::string;
using std::map;
using std::vector;

static TError ValidUint(std::shared_ptr<const TContainer> container, const string &str) {
    uint32_t val;
    if (StringToUint32(str, val))
        return TError(EError::InvalidValue, "invalid numeric value");

    return TError::Success();
}

static TError ValidBool(std::shared_ptr<const TContainer> container, const string &str) {
    if (str != "true" && str != "false")
        return TError(EError::InvalidValue, "invalid boolean value");

    return TError::Success();
}

static TError ValidUser(std::shared_ptr<const TContainer> container, const string &user) {
    TUser u(user);
    return u.Load();
}

static TError ValidGroup(std::shared_ptr<const TContainer> container, const string &group) {
    TGroup g(group);
    return g.Load();
}

static TError ValidMemGuarantee(std::shared_ptr<const TContainer> container, const string &str) {
    uint64_t newval;

    auto memroot = memorySubsystem->GetRootCgroup();
    if (!memroot->HasKnob("memory.low_limit_in_bytes"))
        return TError(EError::NotSupported, "invalid kernel");

    if (StringToUint64(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    if (!container->ValidHierarchicalProperty("memory_guarantee", str))
        return TError(EError::InvalidValue, "invalid hierarchical value");

    uint64_t total = container->GetRoot()->GetChildrenSum("memory_guarantee", container, newval);
    if (total + config().daemon().memory_guarantee_reserve() > GetTotalMemory())
        return TError(EError::ResourceNotAvailable, "can't guarantee all available memory");

    return TError::Success();
}

static TError ValidRecharge(std::shared_ptr<const TContainer> container, const string &str) {
    auto memroot = memorySubsystem->GetRootCgroup();
    if (!memroot->HasKnob("memory.recharge_on_pgfault"))
        return TError(EError::NotSupported, "invalid kernel");

    return ValidBool(container, str);
}

static TError ValidMemLimit(std::shared_ptr<const TContainer> container, const string &str) {
    uint64_t newval;

    if (StringToUint64(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    if (!container->ValidHierarchicalProperty("memory_limit", str))
        return TError(EError::InvalidValue, "invalid hierarchical value");

    return TError::Success();
}

static TError ValidCpuPolicy(std::shared_ptr<const TContainer> container, const string &str) {
    if (str != "normal" && str != "rt" && str != "idle")
        return TError(EError::InvalidValue, "invalid policy");

    if (str == "rt") {
        auto cpuroot = cpuSubsystem->GetRootCgroup();
        if (!cpuroot->HasKnob("cpu.smart"))
            return TError(EError::NotSupported, "invalid kernel");
    }

    if (str == "idle")
        return TError(EError::NotSupported, "not implemented");

    return TError::Success();
}

static TError ValidCpuPriority(std::shared_ptr<const TContainer> container, const string &str) {
    int val;

    if (StringToInt(str, val))
        return TError(EError::InvalidValue, "invalid value");

    if (val < 0 || val > 99)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidNetGuarantee(std::shared_ptr<const TContainer> container, const string &str) {
    uint32_t newval;

    if (StringToUint32(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidNetCeil(std::shared_ptr<const TContainer> container, const string &str) {
    uint32_t newval;

    if (StringToUint32(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidNetPriority(std::shared_ptr<const TContainer> container, const string &str) {
    int val;

    if (StringToInt(str, val))
        return TError(EError::InvalidValue, "invalid value");

    if (val < 0 || val > 7)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidPath(std::shared_ptr<const TContainer> container, const string &str) {
    if (!str.length() || str[0] != '/')
        return TError(EError::InvalidValue, "invalid directory");
    return TError::Success();
}

static TError ExistingFile(std::shared_ptr<const TContainer> container, const string &str) {
    TFile f(str);
    if (!f.Exists())
        return TError(EError::InvalidValue, "file doesn't exist");
    return TError::Success();
}

static TError ValidUlimit(std::shared_ptr<const TContainer> container, const string &str) {
    map<int,struct rlimit> rlim;
    return ParseRlimit(str, rlim);
}

static TError ValidBind(std::shared_ptr<const TContainer> container, const string &str) {
    vector<TBindMap> dirs;
    return ParseBind(str, dirs);
}

static TError ValidNet(std::shared_ptr<const TContainer> container, const string &str) {
    TNetCfg net;
    return ParseNet(container, str, net);
}

#define DEFSTR(S) [](std::shared_ptr<const TContainer> container)->std::string { return S; }

static std::string DefaultStdFile(std::shared_ptr<const TContainer> c,
                                  const std::string &name) {
    string cwd, root;
    TError error = c->GetProperty("cwd", cwd);
    TLogger::LogError(error, "Can't get cwd for std file");
    if (error)
        return "";

    error = c->GetProperty("root", root);
    TLogger::LogError(error, "Can't get root for std file");
    if (error)
        return "";

    string prefix;
    if (c->UseParentNamespace())
        prefix = c->GetName(false) + ".";

    TPath path = root;
    path.AddComponent(cwd);
    path.AddComponent(prefix + name);
    return path.ToString();
}

static std::string TrueWithRoot(std::shared_ptr<const TContainer> c) {
    if (c->IsDefaultProperty("root"))
        return "false";
    else
        return "true";
}

std::map<std::string, const TPropertySpec> propertySpec = {
    { "command",
        {
            "Command executed upon container start",
            DEFSTR("")
        }
    },
    { "user",
        {
            "Start command with given user",
            [](std::shared_ptr<const TContainer> c)->std::string {
                int uid, gid;
                c->GetPerm(uid, gid);
                TUser u(uid);
                if (u.Load())
                    return std::to_string(uid);
                else
                    return u.GetName();
            },
            SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY,
            ValidUser
        }
    },
    { "group",
        {
            "Start command with given group",
            [](std::shared_ptr<const TContainer> c)->std::string {
                int uid, gid;
                c->GetPerm(uid, gid);
                TGroup g(gid);
                if (g.Load())
                    return std::to_string(gid);
                else
                    return g.GetName();
            },
            SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY,
            ValidGroup
        }
    },
    { "env",
        {
            "Container environment variables",
            DEFSTR(""),
            PARENT_DEF_PROPERTY
        }
    },
    { "root",
        {
            "Container root directory",
            DEFSTR("/"),
            PARENT_DEF_PROPERTY
        }
    },
    { "cwd",
        {
            "Container working directory",
            [](std::shared_ptr<const TContainer> c)->std::string {
                if (!c->IsDefaultProperty("root"))
                    return "/";

                return config().container().tmp_dir() + "/" + c->GetName();
            },
            PARENT_DEF_PROPERTY,
            ValidPath,
        }
    },
    { "stdin_path",
        {
            "Container standard input path",
            DEFSTR("/dev/null"),
            0,
            ExistingFile
        }
    },
    { "stdout_limit",
        {
            "Return no more than given number of bytes from standard output/error",
            [](std::shared_ptr<const TContainer> c)->std::string {
                return std::to_string(config().container().stdout_limit());
            },
            0,
            [](std::shared_ptr<const TContainer> c, const string &s)->TError {
                uint32_t val;
                uint32_t max = config().container().stdout_limit();

                TError error = StringToUint32(s, val);
                if (error)
                    return error;

                if (val > max)
                    return TError(EError::InvalidValue,
                                  "Maximum number of bytes: " +
                                  std::to_string(max));

                return TError::Success();
            }
        }
    },
    { "stdout_path",
        {
            "Container standard output path",
            [](std::shared_ptr<const TContainer> c)->std::string {
                return DefaultStdFile(c, "stdout");
            },
            0,
            ValidPath
        }
    },
    { "stderr_path",
        {
            "Container standard error path",
            [](std::shared_ptr<const TContainer> c)->std::string {
                return DefaultStdFile(c, "stderr");
            },
            0,
            ValidPath
        }
    },
    { "memory_guarantee",
        {
            "Guaranteed amount of memory",
            DEFSTR("0"),
            DYNAMIC_PROPERTY | PARENT_RO_PROPERTY,
            ValidMemGuarantee
        }
    },
    { "memory_limit",
        {
            "Memory hard limit",
            DEFSTR("0"),
            DYNAMIC_PROPERTY,
            ValidMemLimit
        }
    },
    { "recharge_on_pgfault",
        {
            "Recharge memory on page fault",
            DEFSTR("false"),
            DYNAMIC_PROPERTY | PARENT_RO_PROPERTY,
            ValidRecharge
        }
    },
    { "cpu_policy",
        {
            "CPU policy: rt, normal, idle",
            DEFSTR("normal"),
            DYNAMIC_PROPERTY | PARENT_RO_PROPERTY,
            ValidCpuPolicy
        }
    },
    { "cpu_priority",
        {
            "CPU priority: 0-99",
            DEFSTR(std::to_string(DEF_CLASS_PRIO)),
            DYNAMIC_PROPERTY | PARENT_RO_PROPERTY,
            ValidCpuPriority
        }
    },
    { "net_guarantee",
        {
            "Guaranteed container network bandwidth",
            DEFSTR(std::to_string(DEF_CLASS_RATE)),
            PARENT_RO_PROPERTY,
            ValidNetGuarantee
        }
    },
    { "net_ceil",
        {
            "Maximum container network bandwidth",
            DEFSTR(std::to_string(DEF_CLASS_CEIL)),
            PARENT_RO_PROPERTY,
            ValidNetCeil
        }
    },
    { "net_priority",
        {
            "Container network priority: 0-7",
            DEFSTR(std::to_string(DEF_CLASS_NET_PRIO)),
            PARENT_RO_PROPERTY,
            ValidNetPriority
        }
    },
    { "respawn",
        {
            "Automatically respawn dead container",
            DEFSTR("false"),
            0,
            ValidBool
        }
    },
    { "max_respawns",
        {
            "Limit respawn count for specific container",
            DEFSTR("-1"),
            0,
            ValidUint
        }
    },
    { "isolate",
        {
            "Isolate container from parent",
            DEFSTR("true"),
            0,
            ValidBool
        }
    },
    { "private",
        {
            "User-defined property",
            DEFSTR(""),
            DYNAMIC_PROPERTY,
            [](std::shared_ptr<const TContainer> c, const string &s)->TError {
                uint32_t max = config().container().private_max();

                if (s.length() > max)
                    return TError(EError::InvalidValue, "Value is too long");

                return TError::Success();
            }

        }
    },
    { "ulimit",
        {
            "Container resource limits",
            DEFSTR(""),
            PARENT_DEF_PROPERTY,
            ValidUlimit
        }
    },
    { "hostname",
        {
            "Container hostname",
            DEFSTR(""),
            0,
        }
    },
    { "bind_dns",
        {
            "Bind /etc/resolv.conf and /etc/hosts of host to container",
            TrueWithRoot,
            0,
            ValidBool
        }
    },
    { "bind",
        {
            "Share host directories with container",
            DEFSTR(""),
            0,
            ValidBind
        }
    },
    { "net",
        {
            "Container network settings",
            DEFSTR("host"),
            0,
            ValidNet
        }
    },
    { "allowed_devices",
        {
            "Devices that container can create/read/write",
            DEFSTR("a *:* rwm"),
            0,
        }
    },
};

bool TContainerSpec::IsDefault(std::shared_ptr<const TContainer> container, const std::string &property) const {
    if (Data.find(property) == Data.end())
        return true;

    return GetDefault(container, property) == Data.at(property);
}

string TContainerSpec::GetDefault(std::shared_ptr<const TContainer> container, const string &property) const {
    if (container->UseParentNamespace() &&
        (propertySpec.at(property).Flags & PARENT_DEF_PROPERTY)) {
        string value;
        TError error = container->GetParent()->GetProperty(property, value);
        TLogger::LogError(error, "Can't get default property from parent");
        if (!error)
            return value;
    }

    return propertySpec.at(property).Default(container);
}

string TContainerSpec::Get(std::shared_ptr<const TContainer> container, const string &property) const {
    if (Data.find(property) == Data.end())
        return GetDefault(container, property);

    return Data.at(property);
}

bool TContainerSpec::IsRoot() const {
    return Name == ROOT_CONTAINER;
}

unsigned int TContainerSpec::GetFlags(const std::string &property) const {
    if (propertySpec.find(property) == propertySpec.end())
        return false;

    return propertySpec.at(property).Flags;
}

TError TContainerSpec::GetRaw(const string &property, string &value) const {
    if (Data.find(property) == Data.end())
        return TError(EError::InvalidValue, "Invalid property");
    value = Data.at(property);
    return TError::Success();
}

TError TContainerSpec::SetRaw(const string &property, const string &value) {
    Data[property] = value;
    TError error(AppendStorage(property, value));
    if (error)
        TLogger::LogError(error, "Can't append property to key-value store");
    return error;
}

TError TContainerSpec::Set(std::shared_ptr<const TContainer> container, const std::string &property, const std::string &value) {
    if (propertySpec.find(property) == propertySpec.end()) {
        TError error(EError::InvalidValue, "property not found");
        TLogger::LogError(error, "Can't set property");
        return error;
    }

    if (propertySpec.at(property).Valid) {
        TError error = propertySpec.at(property).Valid(container, value);
        TLogger::LogError(error, "Can't set property");
        if (error)
            return error;
    }

    return SetRaw(property, value);
}

TError TContainerSpec::Create() {
    kv::TNode node;
    return Storage.SaveNode(Name, node);
}

TError TContainerSpec::Restore(const kv::TNode &node) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        Data[key] = value;
    }

    return SyncStorage();
}

TContainerSpec::~TContainerSpec() {
    if (!IsRoot()) {
        TError error = Storage.RemoveNode(Name);
        TLogger::LogError(error, "Can't remove key-value node " + Name);
    }
}

TError TContainerSpec::SyncStorage() {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    for (auto &kv : Data) {
        auto pair = node.add_pairs();
        pair->set_key(kv.first);
        pair->set_val(kv.second);
    }

    return Storage.SaveNode(Name, node);
}

TError TContainerSpec::AppendStorage(const string& key, const string& value) {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return Storage.AppendNode(Name, node);
}

TError ParseRlimit(const std::string &s, map<int,struct rlimit> &rlim) {
    static const map<string,int> nameToIdx = {
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

    vector<string> limits;
    TError error = SplitString(s, ';', limits);
    if (error)
        return error;

    for (auto &limit : limits) {
        vector<string> nameval;

        (void)SplitString(limit, ':', nameval);
        if (nameval.size() != 2)
            return TError(EError::InvalidValue, "Invalid limits format");

        string name = StringTrim(nameval[0]);
        if (nameToIdx.find(name) == nameToIdx.end())
            return TError(EError::InvalidValue, "Invalid limit " + name);
        int idx = nameToIdx.at(name);

        vector<string> softhard;
        (void)SplitString(StringTrim(nameval[1]), ' ', softhard);
        if (softhard.size() != 2)
            return TError(EError::InvalidValue, "Invalid limits number for " + name);

        rlim_t soft, hard;
        if (softhard[0] == "unlim" || softhard[0] == "unliminted") {
            soft = RLIM_INFINITY;
        } else {
            error = StringToUint64(softhard[0], soft);
            if (error)
                return TError(EError::InvalidValue, "Invalid soft limit for " + name);
        }

        if (softhard[1] == "unlim" || softhard[1] == "unliminted") {
            hard = RLIM_INFINITY;
        } else {
            error = StringToUint64(softhard[1], hard);
            if (error)
                return TError(EError::InvalidValue, "Invalid hard limit for " + name);
        }

        rlim[idx].rlim_cur = soft;
        rlim[idx].rlim_max = hard;
    }

    return TError::Success();
}

TError ParseBind(const std::string &s, vector<TBindMap> &dirs) {
    vector<string> lines;

    TError error = SplitEscapedString(s, ';', lines);
    if (error)
        return error;

    for (auto &line : lines) {
        vector<string> tok;
        TBindMap bindMap;

        error = SplitEscapedString(line, ' ', tok);
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

TError ParseNet(std::shared_ptr<const TContainer> container, const std::string &s, TNetCfg &net) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

    vector<string> lines;
    bool none = false;
    net.Share = false;

    TError error = SplitEscapedString(s, ';', lines);
    if (error)
        return error;

    if (lines.size() == 0)
        return TError(EError::InvalidValue, "Configuration is not specified");

    for (auto &line : lines) {
        if (none)
            return TError(EError::InvalidValue,
                          "none can't be mixed with other types");

        vector<string> settings;

        error = SplitEscapedString(line, ' ', settings);
        if (error)
            return error;

        if (settings.size() == 0)
            return TError(EError::InvalidValue, "Invalid net in: " + line);

        string type = StringTrim(settings[0]);

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

                if (!ValidLink(hnet.Dev))
                    return TError(EError::InvalidValue,
                                  "Invalid host interface " + hnet.Dev);

                net.Host.push_back(hnet);
            }
        } else if (type == "macvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid macvlan in: " + line);

            string master = StringTrim(settings[1]);
            string name = StringTrim(settings[2]);
            string type = "bridge";
            string hw = "";

            if (!ValidLink(master))
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

            int idx = container->GetLink()->FindIndex(master);
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
