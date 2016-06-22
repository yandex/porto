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
#include <sstream>

extern "C" {
#include <linux/capability.h>
}

extern __thread TContainer *CurrentContainer;
extern __thread TClient *CurrentClient;
extern TContainerUser ContainerUser;
extern TContainerGroup ContainerGroup;
extern TContainerMemoryGuarantee ContainerMemoryGuarantee;
extern TContainerMemTotalGuarantee ContainerMemTotalGuarantee;
extern TContainerVirtMode ContainerVirtMode;
extern TContainerCommand ContainerCommand;
extern TContainerCwd ContainerCwd;
extern TContainerStdinPath ContainerStdinPath;
extern TContainerStdoutPath ContainerStdoutPath;
extern TContainerStderrPath ContainerStderrPath;
extern TContainerBindDns ContainerBindDns;
extern TContainerIsolate ContainerIsolate;
extern TContainerRoot ContainerRoot;
extern TContainerNet ContainerNet;
extern TContainerHostname ContainerHostname;
extern TContainerRootRo ContainerRootRo;
extern TContainerEnv ContainerEnv;
extern TContainerBind ContainerBind;
extern TContainerIp ContainerIp;
extern TContainerCapabilities ContainerCapabilities;
extern TContainerDefaultGw ContainerDefaultGw;
extern TContainerResolvConf ContainerResolvConf;
extern TContainerDevices ContainerDevices;
extern TContainerRawRootPid ContainerRawRootPid;
extern TContainerRawLoopDev ContainerRawLoopDev;
extern TContainerRawStartTime ContainerRawStartTime;
extern TContainerRawDeathTime ContainerRawDeathTime;
extern std::map<std::string, TContainerProperty*> ContainerPropMap;

bool TPropertyMap::ParentDefault(std::shared_ptr<TContainer> &c,
                                 const std::string &property) const {
    TError error = GetSharedContainer(c);
    if (error) {
        L_ERR() << "Can't get default for " << property << ": " << error << std::endl;
        return false;
    }

    return HasFlags(property, PARENT_DEF_PROPERTY) && !c->Isolate;
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
        uint64_t rate = config().network().default_guarantee();
        if (c->IsRoot())
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

class TNetTosProperty : public TUintValue, public TContainerValue {
public:
    TNetTosProperty() :
        TUintValue(PERSISTENT_VALUE),
        TContainerValue(P_NET_TOS,
                        "IP TOS") {
        SetFlag(UNSUPPORTED_FEATURE);
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

void RegisterProperties(std::shared_ptr<TRawValueMap> m,
                        std::shared_ptr<TContainer> c) {
    const std::vector<TValue *> properties = {
        new TPortoNamespaceProperty,
        new TStdoutLimitProperty,
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
        new TPrivateProperty,
        new TUlimitProperty,
        new TNetTosProperty,
        new TAgingTimeProperty,
        new TEnablePortoProperty,
        new TWeakProperty,
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

        if (!(CurrentContainer->PropMask & CAPABILITIES_SET)) {
            if (owner.IsRootUser())
                CurrentContainer->Caps = ContainerCapabilities.AllCaps;
            else if (CurrentContainer->VirtMode == VIRT_MODE_OS)
                CurrentContainer->Caps = PermittedCaps;
            else
                CurrentContainer->Caps = 0;
        }

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
    ContainerMemoryGuarantee.Init();
    ContainerMemTotalGuarantee.Init();
    ContainerPropMap[ContainerMemoryGuarantee.Name] = &ContainerMemoryGuarantee;
    ContainerPropMap[ContainerMemTotalGuarantee.Name] = &ContainerMemTotalGuarantee;
    ContainerPropMap[ContainerCommand.Name] = &ContainerCommand;
    ContainerPropMap[ContainerVirtMode.Name] = &ContainerVirtMode;
    ContainerPropMap[ContainerCwd.Name] = &ContainerCwd;
    ContainerPropMap[ContainerStdinPath.Name] = &ContainerStdinPath;
    ContainerPropMap[ContainerStdoutPath.Name] = &ContainerStdoutPath;
    ContainerPropMap[ContainerStderrPath.Name] = &ContainerStderrPath;
    ContainerPropMap[ContainerIsolate.Name] = &ContainerIsolate;
    ContainerPropMap[ContainerBindDns.Name] = &ContainerBindDns;
    ContainerPropMap[ContainerRoot.Name] = &ContainerRoot;
    ContainerPropMap[ContainerNet.Name] = &ContainerNet;
    ContainerPropMap[ContainerHostname.Name] = &ContainerHostname;
    ContainerPropMap[ContainerRootRo.Name] = &ContainerRootRo;
    ContainerPropMap[ContainerEnv.Name] = &ContainerEnv;
    ContainerPropMap[ContainerBind.Name] = &ContainerBind;
    ContainerPropMap[ContainerIp.Name] = &ContainerIp;
    ContainerCapabilities.Init();
    ContainerPropMap[ContainerCapabilities.Name] = &ContainerCapabilities;
    ContainerPropMap[ContainerDefaultGw.Name] = &ContainerDefaultGw;
    ContainerPropMap[ContainerResolvConf.Name] = &ContainerResolvConf;
    ContainerPropMap[ContainerDevices.Name] = &ContainerDevices;
    ContainerPropMap[ContainerRawRootPid.Name] = &ContainerRawRootPid;
    ContainerPropMap[ContainerRawLoopDev.Name] = &ContainerRawLoopDev;
    ContainerPropMap[ContainerRawStartTime.Name] = &ContainerRawStartTime;
    ContainerPropMap[ContainerRawDeathTime.Name] = &ContainerRawDeathTime;
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

TError TContainerProperty::IsAlive(void) {
    auto state = CurrentContainer->GetState();

    if (state == EContainerState::Dead)
        return TError(EError::InvalidState,
                      "Cannot change property while in the dead state");

    return TError::Success();
}

TError TContainerMemoryGuarantee::Set(const std::string &mem_guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_val = 0lu;
    error = StringToSize(mem_guarantee, new_val);
    if (error)
        return error;

    CurrentContainer->CurrentMemGuarantee = new_val;

    uint64_t usage = CurrentContainer->GetRoot()->GetHierarchyMemGuarantee();
    uint64_t total = GetTotalMemory();
    uint64_t reserve = config().daemon().memory_guarantee_reserve();
    if (usage + reserve > total) {
        CurrentContainer->CurrentMemGuarantee = CurrentContainer->MemGuarantee;

        return TError(EError::ResourceNotAvailable,
                "can't guarantee all available memory: requested " +
                std::to_string(new_val) + " (will be " + std::to_string(usage) +
                " of " + std::to_string(total) + ", reserve " + std::to_string(reserve) + ")");
    }

    if (CurrentContainer->GetState() == EContainerState::Running ||
        CurrentContainer->GetState() == EContainerState::Meta ||
        CurrentContainer->GetState() == EContainerState::Paused) {
        auto memcg = CurrentContainer->GetCgroup(MemorySubsystem);
        error = MemorySubsystem.SetGuarantee(memcg, new_val);

        if (error) {
            CurrentContainer->CurrentMemGuarantee = CurrentContainer->MemGuarantee;
            L_ERR() << "Can't set " << P_MEM_GUARANTEE << ": " << error << std::endl;

            return error;
        }
    }

    CurrentContainer->MemGuarantee = new_val;
    CurrentContainer->PropMask |= MEM_GUARANTEE_SET;

    return TError::Success();
}

TError TContainerMemoryGuarantee::Get(std::string &value) {
    value = std::to_string(CurrentContainer->MemGuarantee);

    return TError::Success();
}

TError TContainerMemTotalGuarantee::Get(std::string &value) {
    uint64_t total = CurrentContainer->GetHierarchyMemGuarantee();
    value = std::to_string(total);

    return TError::Success();
}

TError TContainerCommand::Set(const std::string &command) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Command = command;
    CurrentContainer->PropMask |= COMMAND_SET;

    return TError::Success();
}

TError TContainerCommand::Get(std::string &value) {
    std::string virt_mode;

    value = CurrentContainer->Command;

    return TError::Success();
}

TError TContainerVirtMode::Set(const std::string &virt_mode) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (virt_mode == P_VIRT_MODE_APP)
        CurrentContainer->VirtMode = VIRT_MODE_APP;

    else if (virt_mode == P_VIRT_MODE_OS)
        CurrentContainer->VirtMode = VIRT_MODE_OS;

    else
        return TError(EError::InvalidValue, std::string("Unsupported ") +
                      P_VIRT_MODE + ": " + virt_mode);

    if (CurrentContainer->VirtMode == VIRT_MODE_OS) {
        if (!(CurrentContainer->PropMask & CWD_SET)) {

            CurrentContainer->Cwd = "/";
            ContainerCwd.Propagate("/");
        }

        if (!(CurrentContainer->PropMask & COMMAND_SET))
            CurrentContainer->Command = "/sbin/init";

        if (!(CurrentContainer->PropMask & STDOUT_SET))
            CurrentContainer->StdoutPath = "/dev/null";

        if (!(CurrentContainer->PropMask & STDERR_SET))
            CurrentContainer->StderrPath = "/dev/null";

        if (!(CurrentContainer->PropMask & BIND_DNS_SET))
            CurrentContainer->BindDns = false;

        if (!(CurrentContainer->PropMask & NET_SET))
            CurrentContainer->NetProp = { "none" };

        if (!(CurrentContainer->PropMask & CAPABILITIES_SET))
            CurrentContainer->Caps = PermittedCaps;
    }

    CurrentContainer->PropMask |= VIRT_MODE_SET;

    return TError::Success();
}

TError TContainerVirtMode::Get(std::string &value) {

    switch (CurrentContainer->VirtMode) {
        case VIRT_MODE_APP:
            value = P_VIRT_MODE_APP;
            break;
        case VIRT_MODE_OS:
            value = P_VIRT_MODE_OS;
            break;
        default:
            value = "unknown " + std::to_string(CurrentContainer->VirtMode);
            break;
    }

    return TError::Success();
}

TError TContainerStdinPath::Set(const std::string &path) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->StdinPath = path;
    CurrentContainer->PropMask |= STDIN_SET;

    return TError::Success();
}

TError TContainerStdinPath::Get(std::string &value) {
    value = CurrentContainer->StdinPath;

    return TError::Success();
}

TError TContainerStdoutPath::Set(const std::string &path) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->StdoutPath = path;
    CurrentContainer->PropMask |= STDOUT_SET;

    return TError::Success();
}

TError TContainerStdoutPath::Get(std::string &value) {
    value = CurrentContainer->StdoutPath;

    return TError::Success();
}

TError TContainerStderrPath::Set(const std::string &path) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->StderrPath = path;
    CurrentContainer->PropMask |= STDERR_SET;

    return TError::Success();
}

TError TContainerStderrPath::Get(std::string &value) {
    value = CurrentContainer->StderrPath;

    return TError::Success();
}

TError TContainerBindDns::Get(std::string &value) {
    value = CurrentContainer->BindDns ? "true" : "false";

    return TError::Success();
}

TError TContainerBindDns::Set(const std::string &bind_needed) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (bind_needed == "true")
        CurrentContainer->BindDns = true;
    else if (bind_needed == "false")
        CurrentContainer->BindDns = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->PropMask |= BIND_DNS_SET;

    return TError::Success();
}

TError TContainerIsolate::Get(std::string &value) {
    value = CurrentContainer->Isolate ? "true" : "false";

    return TError::Success();
}

TError TContainerIsolate::Set(const std::string &isolate_needed) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (isolate_needed == "true")
        CurrentContainer->Isolate = true;
    else if (isolate_needed == "false")
        CurrentContainer->Isolate = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    if (!CurrentContainer->Isolate) {
        if (!(CurrentContainer->PropMask & BIND_DNS_SET))
            CurrentContainer->BindDns = false;

        auto p = CurrentContainer->GetParent();
        if (p) {
            if (!(CurrentContainer->PropMask & CWD_SET)) {
                CurrentContainer->Cwd = p->Cwd;

                ContainerCwd.Propagate(p->Cwd);
            }
        }

    }

    CurrentContainer->PropMask |= ISOLATE_SET;

    return TError::Success();
}

TError TContainerRoot::Get(std::string &value) {
    value = CurrentContainer->Root;

    return TError::Success();
}

TError TContainerRoot::Set(const std::string &root) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Root = root;

    if (root != "/") {
        if (!(CurrentContainer->PropMask & BIND_DNS_SET) &&
            CurrentContainer->VirtMode != VIRT_MODE_OS &&
            CurrentContainer->Isolate) {
            CurrentContainer->BindDns = true;
        }

        if (!(CurrentContainer->PropMask & CWD_SET)) {
            CurrentContainer->Cwd = "/";
            ContainerCwd.Propagate("/");
        }
    }
    CurrentContainer->PropMask |= ROOT_SET;

    return TError::Success();
}

TError TContainerNet::Set(const std::string &net_desc) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> new_net_desc;
    error = StringToStrList(net_desc, new_net_desc);

    TNetCfg cfg;
    error = cfg.ParseNet(new_net_desc);
    if (error)
        return error;

    CurrentContainer->NetProp = new_net_desc; /* FIXME: Copy vector contents? */

    CurrentContainer->PropMask |= NET_SET;
    return TError::Success();
}

TError TContainerNet::Get(std::string &value) {
    return StrListToString(CurrentContainer->NetProp, value);
}

TError TContainerCwd::Set(const std::string &cwd) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Cwd = cwd;
    Propagate(cwd);
    CurrentContainer->PropMask |= CWD_SET;

    return TError::Success();
}

TError TContainerCwd::Get(std::string &value) {
    value = CurrentContainer->Cwd;

    return TError::Success();
}

void TContainerCwd::Propagate(const std::string &cwd) {

    for (auto iter : CurrentContainer->Children) {
        if (auto child = iter.lock()) {

            auto old = CurrentContainer;
            CurrentContainer = child.get();

            if (!(CurrentContainer->PropMask & CWD_SET) &&
                !(CurrentContainer->Isolate)) {

                CurrentContainer->Cwd = cwd;
                ContainerCwd.Propagate(cwd);
            }
            CurrentContainer = old;
        }
    }
}

TError TContainerRootRo::Set(const std::string &ro) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (ro == "true")
        CurrentContainer->RootRo = true;
    else if (ro == "false")
        CurrentContainer->RootRo = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");
    
    CurrentContainer->PropMask |= ROOT_RDONLY_SET;

    return TError::Success();
}

TError TContainerRootRo::Get(std::string &ro) {
    ro = CurrentContainer->RootRo ? "true" : "false";

    return TError::Success();
}

TError TContainerHostname::Set(const std::string &hostname) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Hostname = hostname;
    CurrentContainer->PropMask |= HOSTNAME_SET;

    return TError::Success();
}

TError TContainerHostname::Get(std::string &value) {
    value = CurrentContainer->Hostname;

    return TError::Success();
}

TError TContainerEnv::Set(const std::string &env_val) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> envs;

    error = StringToStrList(env_val, envs);
    if (error)
        return error;

    TEnv env;
    error =  env.Parse(envs, true);
    if (error)
        return error;

    env.Format(CurrentContainer->EnvCfg);
    CurrentContainer->PropMask |= ENV_SET;

    return TError::Success();
}

TError TContainerEnv::Get(std::string &value) {
    return StrListToString(CurrentContainer->EnvCfg, value);
}

TError TContainerEnv::SetIndexed(const std::string &index, const std::string &env_val) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TEnv env;
    error = env.Parse(CurrentContainer->EnvCfg, true);
    if (error)
        return error;

    error = env.Parse({index + "=" + env_val}, true);
    if (error)
        return error;

    env.Format(CurrentContainer->EnvCfg);
    CurrentContainer->PropMask |= ENV_SET;

    return TError::Success();
}

TError TContainerEnv::GetIndexed(const std::string &index, std::string &value) {
    TEnv env;
    TError error = CurrentContainer->GetEnvironment(env);
    if (error)
        return error;

    if (!env.GetEnv(index, value))
        return TError(EError::InvalidValue, "Variable " + index + " not defined");

    return TError::Success();
}

TError TContainerBind::Set(const std::string &bind_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> binds;
    error = StringToStrList(bind_str, binds);
    if (error)
        return error;

    std::vector<TBindMap> bm;

    for (auto &line : binds) {
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

    CurrentContainer->BindMap = bm;
    CurrentContainer->PropMask |= BIND_SET;

    return TError::Success();
}

TError TContainerBind::Get(std::string &value) {
    std::vector<std::string> bind_str_list;
    for (auto m : CurrentContainer->BindMap)
        bind_str_list.push_back(m.Source.ToString() + " " + m.Dest.ToString() + 
                                (m.Rdonly ? " ro" : " rw"));
    
    return StrListToString(bind_str_list, value);
}

TError TContainerIp::Set(const std::string &ipaddr) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> ipaddrs;
    error = StringToStrList(ipaddr, ipaddrs);
    if (error)
        return error;

    TNetCfg cfg;
    error = cfg.ParseIp(ipaddrs);
    if (error)
        return error;

    CurrentContainer->IpList = ipaddrs;
    CurrentContainer->PropMask |= IP_SET;

    return TError::Success();
}

TError TContainerIp::Get(std::string &value) {
    return StrListToString(CurrentContainer->IpList, value);
}

TError TContainerCapabilities::Set(const std::string &caps_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (!CurrentClient->Cred.IsRootUser())
        return TError(EError::Permission, "Only root can change this property");

    std::vector<std::string> caps;
    error = StringToStrList(caps_str, caps);
    if (error)
        return error;

    uint64_t cap_mask = 0lu;

    for (auto &cap: caps) {
        if (SupportedCaps.find(cap) == SupportedCaps.end())
            return TError(EError::InvalidValue,
                    "Unsupported capability " + cap);

        if (SupportedCaps.at(cap) > LastCapability)
            return TError(EError::InvalidValue,
                    "Unsupported kernel capability " + cap);

        cap_mask |= CAP_MASK(SupportedCaps.at(cap));

    }
    CurrentContainer->Caps = cap_mask;
    CurrentContainer->PropMask |= CAPABILITIES_SET;

    return TError::Success();
}

TError TContainerCapabilities::Get(std::string &value) {
   std::vector<std::string> caps;

    for (const auto &cap : SupportedCaps)
        if (CurrentContainer->Caps & CAP_MASK(cap.second))
            caps.push_back(cap.first);

    return StrListToString(caps, value);
}

TError TContainerDefaultGw::Set(const std::string &gw) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TNetCfg cfg;
    std::vector<std::string> gws;

    error = StringToStrList(gw, gws);
    if (error)
        return error;

    error = cfg.ParseGw(gws);
    if (error)
        return error;

    CurrentContainer->DefaultGw = gws;
    CurrentContainer->PropMask |= DEFAULT_GW_SET;

    return TError::Success();
}

TError TContainerDefaultGw::Get(std::string &value) {
    return StrListToString(CurrentContainer->DefaultGw, value);
}

TError TContainerResolvConf::Set(const std::string &conf_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> conf;

    error = StringToStrList(conf_str, conf);
    if (error)
        return error;

    CurrentContainer->ResolvConf = conf;
    CurrentContainer->PropMask |= RESOLV_CONF_SET;

    return TError::Success();
}

TError TContainerResolvConf::Get(std::string &value) {
    return StrListToString(CurrentContainer->ResolvConf, value);
}

TError TContainerDevices::Set(const std::string &dev_str) {
    std::vector<std::string> dev_list;

    TError error = StringToStrList(dev_str, dev_list);
    if (error)
        return error;

    CurrentContainer->Devices = dev_list;
    CurrentContainer->PropMask |= DEVICES_SET;

    return TError::Success();
}

TError TContainerDevices::Get(std::string &value) {
    return StrListToString(CurrentContainer->Devices, value);
}

TError TContainerRawRootPid::SetFromRestore(const std::string &value) {
    std::vector<std::string> str_list;
    TError error = StringToStrList(value, str_list);
    if (error)
        return error;

    return StringsToIntegers(str_list, CurrentContainer->RootPid);
}

TError TContainerRawRootPid::Get(std::string &value) {
    std::stringstream str;
    bool first = true;

    for (auto v : CurrentContainer->RootPid) {
        if (first)
             first = false;
        else
             str << ";";
        str << v;
   }

   value = str.str();
   return TError::Success();
}

TError TContainerRawLoopDev::SetFromRestore(const std::string &value) {
    return StringToInt(value, CurrentContainer->LoopDev);
}

TError TContainerRawLoopDev::Get(std::string &value) {
    value = std::to_string(CurrentContainer->LoopDev);

    return TError::Success();
}

TError TContainerRawStartTime::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CurrentContainer->StartTime);
}

TError TContainerRawStartTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->StartTime);

    return TError::Success();
}

TError TContainerRawDeathTime::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CurrentContainer->DeathTime);
}

TError TContainerRawDeathTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->DeathTime);

    return TError::Success();
}
