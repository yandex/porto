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

static TError ValidBool(std::shared_ptr<const TContainer> container, const string str) {
    if (str != "true" && str != "false")
        return TError(EError::InvalidValue, "invalid boolean value");

    return TError::Success();
}

static TError ValidUser(std::shared_ptr<const TContainer> container, const string user) {
    TUser u(user);
    return u.Load();
}

static TError ValidGroup(std::shared_ptr<const TContainer> container, const string group) {
    TGroup g(group);
    return g.Load();
}

static TError ValidMemGuarantee(std::shared_ptr<const TContainer> container, const string str) {
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

static TError ValidRecharge(std::shared_ptr<const TContainer> container, const string str) {
    auto memroot = memorySubsystem->GetRootCgroup();
    if (!memroot->HasKnob("memory.recharge_on_pgfault"))
        return TError(EError::NotSupported, "invalid kernel");

    return ValidBool(container, str);
}

static TError ValidMemLimit(std::shared_ptr<const TContainer> container, const string str) {
    uint64_t newval;

    if (StringToUint64(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    if (!container->ValidHierarchicalProperty("memory_limit", str))
        return TError(EError::InvalidValue, "invalid hierarchical value");

    return TError::Success();
}

static TError ValidCpuPolicy(std::shared_ptr<const TContainer> container, const string str) {
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

static TError ValidCpuPriority(std::shared_ptr<const TContainer> container, const string str) {
    int val;

    if (StringToInt(str, val))
        return TError(EError::InvalidValue, "invalid value");

    if (val < 0 || val > 99)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidNetGuarantee(std::shared_ptr<const TContainer> container, const string str) {
    uint32_t newval;

    if (StringToUint32(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidNetCeil(std::shared_ptr<const TContainer> container, const string str) {
    uint32_t newval;

    if (StringToUint32(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidNetPriority(std::shared_ptr<const TContainer> container, const string str) {
    int val;

    if (StringToInt(str, val))
        return TError(EError::InvalidValue, "invalid value");

    if (val < 0 || val > 7)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidIsolate(std::shared_ptr<const TContainer> container, const string str) {
    if (str != "true" && str != "false" && str != "parent")
        return TError(EError::InvalidValue, "invalid isolate value");

    return TError::Success();
}

#define DEFSTR(S) [](std::shared_ptr<const TContainer> container)->std::string { return S; }

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
            DEFSTR(""),
            CGNSREQ_PROPERTY | SUPERUSER_PROPERTY,
            ValidUser
        }
    },
    { "group",
        {
            "Start command with given group",
            DEFSTR(""),
            CGNSREQ_PROPERTY | SUPERUSER_PROPERTY,
            ValidGroup
        }
    },
    { "env",
        {
            "Container environment variables",
            DEFSTR("")
        }
    },
    /*
    { "root",
        {
            "Container root directory",
            DEFSTR("/")
        }
    },
    */
    { "cwd",
        {
            "Container working directory",
            DEFSTR(""),
            CGNSREQ_PROPERTY
        }
    },
    {
        "stdin_path",
        {
            "Container standard input path",
            DEFSTR("")
        }
    },
    {
        "stdout_path",
        {
            "Container standard output path",
            DEFSTR("")
        }
    },
    {
        "stderr_path",
        {
            "Container standard error path",
            DEFSTR("")
        }
    },
    {
        "memory_guarantee",
        {
            "Guaranteed amount of memory",
            DEFSTR("0"),
            CGNSREQ_PROPERTY | DYNAMIC_PROPERTY,
            ValidMemGuarantee
        }
    },
    {
        "memory_limit",
        {
            "Memory hard limit",
            DEFSTR("0"),
            CGNSREQ_PROPERTY | DYNAMIC_PROPERTY,
            ValidMemLimit
        }
    },
    {
        "recharge_on_pgfault",
        {
            "Recharge memory on page fault",
            DEFSTR("false"),
            CGNSREQ_PROPERTY | DYNAMIC_PROPERTY,
            ValidRecharge
        }
    },
    {
        "cpu_policy",
        {
            "CPU policy: rt, normal, idle",
            DEFSTR("normal"),
            CGNSREQ_PROPERTY,
            ValidCpuPolicy
        }
    },
    {
        "cpu_priority",
        {
            "CPU priority: 0-99",
            DEFSTR(std::to_string(DEF_CLASS_PRIO)),
            CGNSREQ_PROPERTY | DYNAMIC_PROPERTY,
            ValidCpuPriority
        }
    },
    {
        "net_guarantee",
        {
            "Guaranteed container network bandwidth",
            DEFSTR(std::to_string(DEF_CLASS_RATE)),
            CGNSREQ_PROPERTY,
            ValidNetGuarantee
        }
    },
    {
        "net_ceil",
        {
            "Maximum container network bandwidth",
            DEFSTR(std::to_string(DEF_CLASS_CEIL)),
            CGNSREQ_PROPERTY,
            ValidNetCeil
        }
    },
    {
        "net_priority",
        {
            "Container network priority: 0-7",
            DEFSTR(std::to_string(DEF_CLASS_NET_PRIO)),
            CGNSREQ_PROPERTY,
            ValidNetPriority
        }
    },
    {
        "respawn",
        {
            "Automatically respawn dead container",
            DEFSTR("false"),
            0,
            ValidBool
        }
    },
    {
        "isolate",
        {
            "Isolate container from others",
            DEFSTR("true"),
            0,
            ValidIsolate
        }
    },
};

string TContainerSpec::Get(std::shared_ptr<const TContainer> container, const string &property) const {
    if (Data.find(property) == Data.end())
        return propertySpec.at(property).Default(container);

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
