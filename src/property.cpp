#include "property.hpp"
#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "client.hpp"
#include "rpc.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "network.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/proc.hpp"
#include "util/cred.hpp"
#include <sstream>

extern "C" {
#include <sys/sysinfo.h>
#include <sys/wait.h>
}

__thread TContainer *CT = nullptr;
std::map<std::string, TProperty*> ContainerProperties;

void LoadMap(const rpc::TUintMap &value, const TUintMap& current, TUintMap &result) {
    if (value.merge())
        result = current;
    for (const auto &kv : value.map())
        result[kv.key()] = kv.val();
}

void DumpMap(const TUintMap &value, rpc::TUintMap &result) {
    for (const auto &it : value) {
        auto kv = result.add_map();
        kv->set_key(it.first);
        kv->set_val(it.second);
    }
}

TProperty::TProperty(std::string name, EProperty prop, std::string desc) {
    Name = name;
    Prop = prop;
    Desc = desc;
    ContainerProperties[name] = this;
}

TError TProperty::Has() {
    return OK;
}

TError TProperty::Set(const std::string &) {
    return TError(EError::NotSupported, "Not implemented: " + Name);
}

TError TProperty::GetIndexed(const std::string &, std::string &) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

bool TProperty::Has(const rpc::TContainerSpec &) {
    return false;
}

TError TProperty::Load(const rpc::TContainerSpec &) {
    return OK;
}

void TProperty::Dump(rpc::TContainerSpec &) {
}

void TProperty::Dump(rpc::TContainerStatus &) {
}

TError TProperty::Save(std::string &value) {
    return Get(value);
}

void TProperty::DumpIndexed(const std::string &, rpc::TContainerSpec &) {
}

void TProperty::DumpIndexed(const std::string &, rpc::TContainerStatus &) {
}

TError TProperty::SetIndexed(const std::string &, const std::string &) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

std::string TProperty::GetDesc() const {
    auto desc = Desc;
    if (IsReadOnly)
        desc += " (ro)";
    if (IsDynamic)
        desc += " (dynamic)";
    return desc;
}

TError TProperty::CanGet() const {
    PORTO_ASSERT(CT->IsStateLockedRead());

    if (!IsSupported)
        return TError(EError::NotSupported, "{} is not supported", Name);

    if (IsRuntimeOnly && (CT->State == EContainerState::Stopped ||
                          CT->State == EContainerState::Starting))
        return TError(EError::InvalidState, "{} is not available in {} state", Name, TContainer::StateName(CT->State));

    if (IsDeadOnly && CT->State != EContainerState::Dead)
        return TError(EError::InvalidState, "{} available only in dead state", Name);

    return OK;
}

TError TProperty::CanSet() const {
    PORTO_ASSERT(CT->IsStateLockedWrite());

    if (!IsSupported)
        return TError(EError::NotSupported, "{} is not supported", Name);

    if (IsReadOnly)
        return TError(EError::InvalidValue, "{} is raad-only", Name);

    if (!IsDynamic && CT->State != EContainerState::Stopped)
        return TError(EError::InvalidState, "{} could be set only in stopped state", Name);

    if (!IsAnyState && CT->State == EContainerState::Dead)
        return TError(EError::InvalidState, "{} cannot be set in dead state", Name);

    return OK;
}

TError TProperty::Start(void) {
    return OK;
}

class TCapLimit : public TProperty {
public:
    TCapLimit() : TProperty(P_CAPABILITIES, EProperty::CAPABILITIES,
            "Limit capabilities in container: SYS_ADMIN;NET_ADMIN;... see man capabilities") {}

    TError CommitLimit(TCapabilities &limit) {
        if (limit.Permitted & ~AllCapabilities.Permitted) {
            limit.Permitted &= ~AllCapabilities.Permitted;
            return TError(EError::InvalidValue,
                          "Unsupported capability: " + limit.Format());
        }

        CT->CapLimit = limit;
        CT->SetProp(EProperty::CAPABILITIES);
        CT->TaintFlags.SysBootForIsolated = false;
        CT->SanitizeCapabilitiesAll();
        return OK;
    }

    TError Get(std::string &value) {
        value = CT->CapLimit.Format();
        return OK;
    }

    TError Set(const std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(value);
        if (error)
            return error;
        return CommitLimit(caps);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(index);
        if (error)
            return error;
        value = BoolToString((CT->CapLimit.Permitted &
                              caps.Permitted) == caps.Permitted);
        return OK;
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TCapabilities caps;
        bool val;

        TError error = caps.Parse(index);
        if (!error)
            error = StringToBool(value, val);
        if (error)
            return error;
        if (val)
            caps.Permitted = CT->CapLimit.Permitted | caps.Permitted;
        else
            caps.Permitted = CT->CapLimit.Permitted & ~caps.Permitted;
        return CommitLimit(caps);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        CT->CapLimit.Dump(*spec.mutable_capabilities());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_capabilities();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TCapabilities caps;
        TError error = caps.Load(spec.capabilities());
        if (error)
            return error;

        return CommitLimit(caps);
    }
} static Capabilities;

class TCapAmbient : public TProperty {
public:
    TCapAmbient() : TProperty(P_CAPABILITIES_AMBIENT, EProperty::CAPABILITIES_AMBIENT,
            "Raise capabilities in container: NET_BIND_SERVICE;SYS_PTRACE;...") {}

    void Init(void) {
        IsSupported = HasAmbientCapabilities;
    }

    TError CommitAmbient(TCapabilities &ambient) {
        if (ambient.Permitted & ~AllCapabilities.Permitted) {
            ambient.Permitted &= ~AllCapabilities.Permitted;
            return TError(EError::InvalidValue,
                          "Unsupported capability: " + ambient.Format());
        }

        CT->CapAmbient = ambient;
        CT->SetProp(EProperty::CAPABILITIES_AMBIENT);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }

    TError Get(std::string &value) {
        value = CT->CapAmbient.Format();
        return OK;
    }

    TError Set(const std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(value);
        if (error)
            return error;
        return CommitAmbient(caps);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(index);
        if (error)
            return error;
        value = BoolToString((CT->CapAmbient.Permitted &
                              caps.Permitted) == caps.Permitted);
        return OK;
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TCapabilities caps;
        bool val;

        TError error = caps.Parse(index);
        if (!error)
            error = StringToBool(value, val);
        if (error)
            return error;
        if (val)
            caps.Permitted = CT->CapAmbient.Permitted | caps.Permitted;
        else
            caps.Permitted = CT->CapAmbient.Permitted & ~caps.Permitted;
        return CommitAmbient(caps);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        CT->CapAmbient.Dump(*spec.mutable_capabilities_ambient());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_capabilities_ambient();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TCapabilities caps;
        TError error = caps.Load(spec.capabilities_ambient());
        if (error)
            return error;
        return CommitAmbient(caps);
    }
} static CapabilitiesAmbient;

class TCapAllowed : public TProperty {
public:
    TCapAllowed() : TProperty(P_CAPABILITIES_ALLOWED, EProperty::NONE,
            "Allowed capabilities in container")
    {
        IsReadOnly = true;
    }

    TError Get(std::string &value) {
        value = CT->CapBound.Format();
        return OK;
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(index);
        if (error)
            return error;
        value = BoolToString((CT->CapBound.Permitted &
                              caps.Permitted) == caps.Permitted);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        CT->CapBound.Dump(*spec.mutable_capabilities_allowed());
    }
} static CapAllowed;

class TCapAmbientAllowed : public TProperty {
public:
    TCapAmbientAllowed() : TProperty(P_CAPABILITIES_AMBIENT_ALLOWED, EProperty::NONE,
            "Allowed ambient capabilities in container")
    {
        IsReadOnly = true;
    }

    void Init(void) {
        IsSupported = HasAmbientCapabilities;
    }

    TError Get(std::string &value) {
        value = CT->CapAllowed.Format();
        return OK;
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TCapabilities caps;
        TError error = caps.Parse(index);
        if (error)
            return error;
        value = BoolToString((CT->CapAllowed.Permitted &
                              caps.Permitted) == caps.Permitted);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        CT->CapAllowed.Dump(*spec.mutable_capabilities_ambient_allowed());
    }
} static CapAmbientAllowed;

class TCwd : public TProperty {
public:
    TCwd() : TProperty(P_CWD, EProperty::CWD, "Container working directory") {}
    TError Get(std::string &value) {
        value = CT->GetCwd().ToString();
        return OK;
    }
    TError Set(const std::string &cwd) {
        CT->Cwd = cwd;
        CT->SetProp(EProperty::CWD);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::CWD))
            CT->Cwd = "/";
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_cwd(CT->GetCwd().ToString());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_cwd();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.cwd());
    }
} static Cwd;

class TUlimitProperty : public TProperty {
public:
    TUlimitProperty() : TProperty(P_ULIMIT, EProperty::ULIMIT,
            "Process limits: as|core|data|locks|memlock|nofile|nproc|stack: [soft]|unlimited [hard];... (see man prlimit)") 
    {
        IsDynamic = true;
    }

    TError Get(std::string &value) {
        value = CT->Ulimit.Format();
        return OK;
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        auto type = TUlimit::GetType(index);
        for (auto &res: CT->Ulimit.Resources) {
            if (res.Type == type)
                value = res.Format();
        }
        return OK;
    }

    TError Set(const std::string &value) {
        TUlimit lim;
        TError error = lim.Parse(value);
        if (error)
            return error;
        CT->Ulimit = lim;
        CT->SetProp(EProperty::ULIMIT);
        return OK;
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TUlimit lim;
        TError error = lim.Parse(index + ":" + value);
        if (error)
            return error;
        CT->Ulimit.Merge(lim);
        CT->SetProp(EProperty::ULIMIT);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        TUlimit ulimit = CT->GetUlimit();
        auto map = spec.mutable_ulimit();
        for (auto &res: ulimit.Resources) {
            auto u = map->add_ulimit();
            u->set_type(TUlimit::GetName(res.Type));
            if (res.Soft < RLIM_INFINITY)
                u->set_soft(res.Soft);
            else
                u->set_unlimited(true);
            if (res.Hard < RLIM_INFINITY)
                u->set_hard(res.Hard);
            if (!res.Overwritten)
                u->set_inherited(true);
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_ulimit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TUlimit ulimit;
        if (spec.ulimit().merge())
            ulimit = CT->Ulimit;
        for (auto u: spec.ulimit().ulimit()) {
            auto type = TUlimit::GetType(u.type());
            if (type < 0)
                return TError(EError::InvalidValue, "Invalid ulimit: {}", u.type());
            ulimit.Set(type, u.has_soft() ? u.soft() : RLIM_INFINITY,
                       u.has_hard() ? u.hard() : RLIM_INFINITY, true);
        }
        CT->Ulimit = ulimit;
        CT->SetProp(EProperty::ULIMIT);
        return OK;
    }
} static Ulimit;

class TCpuPolicy : public TProperty {
public:
    TCpuPolicy() : TProperty(P_CPU_POLICY, EProperty::CPU_POLICY,
            "CPU policy: rt, high, normal, batch, idle")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = CT->CpuPolicy;
        return OK;
    }
    TError Set(const std::string &policy) {
        if (policy != "rt" && policy != "high" && policy != "normal" &&
                policy != "batch"  && policy != "idle" && policy != "iso")
            return TError(EError::InvalidValue, "Unknown cpu policy: " + policy);
        if (CT->CpuPolicy != policy) {
            CT->CpuPolicy = policy;
            CT->SetProp(EProperty::CPU_POLICY);
            CT->ChooseSchedPolicy();
        }
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_cpu_policy(CT->CpuPolicy);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_cpu_policy();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.cpu_policy());
    }
} static CpuPolicy;

class TIoPolicy : public TProperty {
public:
    TIoPolicy() : TProperty(P_IO_POLICY, EProperty::IO_POLICY,
            "IO policy: none | rt | high | normal | batch | idle")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = CT->IoPolicy;
        return OK;
    }
    TError Set(const std::string &policy) {
        int ioprio;

        if (policy == "" || policy == "none")
            ioprio = 0;
        else if (policy == "rt")
            ioprio = (1 << 13) | 4;
        else if (policy == "high")
            ioprio = 2 << 13;
        else if (policy == "normal")
            ioprio = (2 << 13) | 4;
        else if (policy == "batch")
            ioprio = (2 << 13) | 7;
        else if (policy == "idle")
            ioprio = 3 << 13;
        else
            return TError(EError::InvalidValue, "invalid policy: " + policy);

        if (CT->IoPolicy != policy) {
            CT->IoPolicy = policy;
            CT->IoPrio = ioprio;
            CT->SetProp(EProperty::IO_POLICY);
        }

        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_io_policy(CT->IoPolicy);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_io_policy();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.io_policy());
    }
} static IoPolicy;

class TIoWeight : public TProperty {
public:
    TIoWeight() : TProperty(P_IO_WEIGHT, EProperty::IO_WEIGHT,
            "IO weight: 0.01..100, default is 1")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_BLKIO;
    }
    TError Get(std::string &value) {
        value = StringFormat("%lg", CT->IoWeight);
        return OK;
    }

    TError Set(double value) {
        if (value < 0.01 || value > 100)
            return TError(EError::InvalidValue, "out of range");
        if (CT->IoWeight != value) {
            CT->IoWeight = value;
            CT->SetProp(EProperty::IO_WEIGHT);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        double val;
        std::string unit;
        TError error = StringToValue(value, val, unit);
        if (error)
            return error;

        if (unit.size())
            return TError(EError::InvalidValue, "out of range");

        return Set(val);
    }

     void Dump(rpc::TContainerSpec &spec) override {
         spec.set_io_weight(CT->IoWeight);
     }

     bool Has(const rpc::TContainerSpec &spec) override {
         return spec.has_io_weight();
     }

     TError Load(const rpc::TContainerSpec &spec) override {
         return Set(spec.io_weight());
     }
} static IoWeight;

class TTaskCred : public TProperty {
public:
    TTaskCred() : TProperty(P_TASK_CRED, EProperty::NONE,
            "Credentials: uid gid groups...") {}
    TError Get(std::string &value) {
        value = fmt::format("{} {}", CT->TaskCred.GetUid(), CT->TaskCred.GetGid());
        for (auto gid: CT->TaskCred.Groups)
            value += fmt::format(" {}", gid);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error;
        TCred cred;
        error = cred.Init(value);
        if (error)
            return error;
        CT->TaskCred = cred;
        CT->SetProp(EProperty::USER);
        CT->SetProp(EProperty::GROUP);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        CT->TaskCred.Dump(*spec.mutable_task_cred());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_task_cred();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return CT->TaskCred.Load(spec.task_cred());
    }
} static TaskCred;

class TUser : public TProperty {
public:
    TUser() : TProperty(P_USER, EProperty::USER,
            "Start command with given user") {}
    TError Get(std::string &value) {
        value = CT->TaskCred.User();
        return OK;
    }
    TError Set(const std::string &username) {
        TCred cred;
        TError error = cred.Init(username);
        if (error) {
            cred.SetGid(CT->TaskCred.GetGid());
            uid_t newUid;
            error = UserId(username, newUid);
            if (error)
                return error;
            cred.SetUid(newUid);
        } else if (CT->HasProp(EProperty::GROUP))
            cred.SetGid(CT->TaskCred.GetGid());
        CT->TaskCred = cred;
        CT->SetProp(EProperty::USER);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode)
            CT->TaskCred.SetUid(RootUser);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_user(CT->TaskCred.User());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_user();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.user());
    }
} static User;

class TGroup : public TProperty {
public:
    TGroup() : TProperty(P_GROUP, EProperty::GROUP, "Start command with given group") {}
    TError Get(std::string &value) {
        value = CT->TaskCred.Group();
        return OK;
    }
    TError Set(const std::string &groupname) {
        gid_t newGid;
        TError error = GroupId(groupname, newGid);
        if (error)
            return error;
        CT->TaskCred.SetGid(newGid);
        CT->SetProp(EProperty::GROUP);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode)
            CT->TaskCred.SetGid(RootGroup);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_group(CT->TaskCred.Group());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_group();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.group());
    }
} static Group;

class TOwnerCred : public TProperty {
public:
    TOwnerCred() : TProperty(P_OWNER_CRED, EProperty::NONE,
            "Owner credentials: uid gid groups...") {}
    TError Get(std::string &value) {
        value = fmt::format("{} {}", CT->OwnerCred.GetUid(), CT->OwnerCred.GetGid());
        for (auto gid: CT->OwnerCred.Groups)
            value += fmt::format(" {}", gid);
        return OK;
    }
    TError SetCred(const TCred &cred) {
        TError error = CL->CanControl(cred);
        if (error)
            return error;
        CT->OwnerCred = cred;
        CT->SetProp(EProperty::OWNER_USER);
        CT->SetProp(EProperty::OWNER_GROUP);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }

    TError Set(const std::string &value) {
        TCred cred;
        TError error = cred.Init(value);
        if (error)
            return error;
        return SetCred(cred);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        CT->OwnerCred.Dump(*spec.mutable_owner_cred());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_owner_cred();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TCred cred;
        TError error = cred.Load(spec.owner_cred());
        if (error)
            return error;
        return SetCred(cred);
    }
} static OwnerCred;

class TOwnerUser : public TProperty {
public:
    TOwnerUser() : TProperty(P_OWNER_USER, EProperty::OWNER_USER,
            "Container owner user") {}

    TError Get(std::string &value) {
        value = CT->OwnerCred.User();
        return OK;
    }

    TError Set(const std::string &username) {
        TCred newCred;
        gid_t oldGid = CT->OwnerCred.GetGid();
        TError error = newCred.Init(username);
        if (error)
            return error;

        /* try to preserve current group if possible */
        if (newCred.IsMemberOf(oldGid) ||
                CL->Cred.IsMemberOf(oldGid) ||
                CL->IsSuperUser())
            newCred.SetGid(oldGid);

        error = CL->CanControl(newCred);
        if (error)
            return error;

        CT->OwnerCred = newCred;
        CT->SetProp(EProperty::OWNER_USER);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_owner_user(CT->OwnerCred.User());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_owner_user();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.owner_user());
    }
} static OwnerUser;

class TOwnerGroup : public TProperty {
public:
    TOwnerGroup() : TProperty(P_OWNER_GROUP, EProperty::OWNER_GROUP,
            "Container owner group") {}

    TError Get(std::string &value) {
        value = CT->OwnerCred.Group();
        return OK;
    }

    TError Set(const std::string &groupname) {
        gid_t newGid;
        TError error = GroupId(groupname, newGid);
        if (error)
            return error;

        if (!CT->OwnerCred.IsMemberOf(newGid) &&
                !CL->Cred.IsMemberOf(newGid) &&
                !CL->IsSuperUser())
            return TError(EError::Permission, "Desired group : " + groupname +
                    " isn't in current user supplementary group list");

        CT->OwnerCred.SetGid(newGid);
        CT->SetProp(EProperty::OWNER_GROUP);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_owner_group(CT->OwnerCred.Group());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_owner_group();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.owner_group());
    }
} static OwnerGroup;

class TMemoryGuarantee : public TProperty {
public:
    TMemoryGuarantee() : TProperty(P_MEM_GUARANTEE, EProperty::MEM_GUARANTEE,
            "Guaranteed amount of memory [bytes]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->MemGuarantee);
        return OK;
    }

    TError Set(uint64_t new_val) {
        CT->NewMemGuarantee = new_val;
        if (CT->State != EContainerState::Stopped) {
            TError error = CT->CheckMemGuarantee();
            /* always allow to decrease guarantee under overcommit */
            if (error && new_val > CT->MemGuarantee) {
                Statistics->FailMemoryGuarantee++;
                CT->NewMemGuarantee = CT->MemGuarantee;
                return error;
            }
        }
        if (CT->MemGuarantee != new_val) {
            CT->MemGuarantee = new_val;
            CT->SetProp(EProperty::MEM_GUARANTEE);
        }
        return OK;
    }

    TError Set(const std::string &mem_guarantee) {
        uint64_t new_val;
        TError error = StringToSize(mem_guarantee, new_val);
        if (error)
            return error;

        return Set(new_val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_memory_guarantee(CT->MemGuarantee);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_memory_guarantee();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
      return Set(spec.memory_guarantee());
    }
} static MemoryGuarantee;

class TMemTotalGuarantee : public TProperty {
public:
    TMemTotalGuarantee() : TProperty(P_MEM_GUARANTEE_TOTAL, EProperty::NONE,
            "Total memory guarantee for container hierarchy")
    {
        IsReadOnly = true;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->GetTotalMemGuarantee());
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_memory_guarantee_total(CT->GetTotalMemGuarantee());
    }
} static MemTotalGuarantee;

class TCommand : public TProperty {
public:
    TCommand() : TProperty(P_COMMAND, EProperty::COMMAND,
            "Command executed upon container start") {}
    TError Get(std::string &command) {
        command = CT->Command;
        return OK;
    }
    TError Set(const std::string &command) {
        if (command.size() > CONTAINER_COMMAND_MAX)
            return TError(EError::InvalidValue, "Command too long, max {}", CONTAINER_COMMAND_MAX);
        CT->Command = command;
        CT->SetProp(EProperty::COMMAND);
        CT->CommandArgv.clear();
        CT->ClearProp(EProperty::COMMAND_ARGV);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::COMMAND))
            CT->Command = "/sbin/init";
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_command(CT->Command);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_command();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.command());
    }
} static Command;

class TCommandArgv : public TProperty {
public:
    TCommandArgv() : TProperty(P_COMMAND_ARGV, EProperty::COMMAND_ARGV,
            "Verbatim command line, format: argv0\\targv1\\t...") {}
    TError Get(std::string &val) {
        val = MergeEscapeStrings(CT->CommandArgv, '\t');
        return OK;
    }
    TError SetCommand() {
        CT->Command = "";
        for (auto &argv: CT->CommandArgv)
            CT->Command += "'" + StringReplaceAll(argv, "'", "'\\''") + "' ";
        CT->SetProp(EProperty::COMMAND);
        return OK;
    }
    TError Set(const std::string &val) {
        if (val.size() > CONTAINER_COMMAND_MAX)
            return TError(EError::InvalidValue, "Command too long, max {}", CONTAINER_COMMAND_MAX);
        CT->CommandArgv = SplitEscapedString(val, '\t');
        if (CT->CommandArgv.size())
            CT->SetProp(EProperty::COMMAND_ARGV);
        else
            CT->ClearProp(EProperty::COMMAND_ARGV);
        return SetCommand();
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        uint64_t i;
        if (StringToUint64(index, i) || i >= CT->CommandArgv.size())
            return TError(EError::InvalidProperty, "Invalid index");
        value = CT->CommandArgv[i];
        return OK;
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        uint64_t i;
        if (StringToUint64(index, i))
            return TError(EError::InvalidProperty, "Invalid index");

        size_t size = CT->Command.size() + value.size();
        if (i < CT->CommandArgv.size())
            size -= CT->CommandArgv[i].size();
        else
            size += i - CT->CommandArgv.size();
        if (i > CONTAINER_COMMAND_MAX || size > CONTAINER_COMMAND_MAX)
            return TError(EError::InvalidValue, "Command too long, max {}", CONTAINER_COMMAND_MAX);

        if (i >= CT->CommandArgv.size())
            CT->CommandArgv.resize(i + 1);

        CT->CommandArgv[i] = value;
        CT->SetProp(EProperty::COMMAND_ARGV);
        return SetCommand();
    }
    void Dump(rpc::TContainerSpec &spec) override {
        if (CT->CommandArgv.empty())
            return;

        auto cmd = spec.mutable_command_argv();
        for (auto &argv: CT->CommandArgv)
            cmd->add_argv(argv);
    }
    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_command_argv();
    }
    TError Load(const rpc::TContainerSpec &spec) override {
        size_t size = 0;
        for (auto &argv: spec.command_argv().argv())
            size += argv.size();
        if (size > CONTAINER_COMMAND_MAX)
            return TError(EError::InvalidValue, "Command too long, max {}", CONTAINER_COMMAND_MAX);
        CT->CommandArgv.clear();
        for (auto &argv: spec.command_argv().argv())
            CT->CommandArgv.push_back(argv);
        if (CT->CommandArgv.size())
            CT->SetProp(EProperty::COMMAND_ARGV);
        else
            CT->ClearProp(EProperty::COMMAND_ARGV);
        return SetCommand();
    }
} static CommandArgv;

class TCoreCommand : public TProperty {
public:
    TCoreCommand() : TProperty(P_CORE_COMMAND, EProperty::CORE_COMMAND,
            "Command for receiving core dump")
    {
        IsDynamic = true;
    }
    void Init(void) {
        IsSupported = config().core().enable();
    }
    TError Get(std::string &command) {
        /* inherit default core command from parent but not across chroot */
        for (auto ct = CT; ct; ct = ct->Parent.get()) {
            command = ct->CoreCommand;
            if (ct->HasProp(EProperty::CORE_COMMAND) || ct->Root != "/")
                break;
        }
        return OK;
    }
    TError Set(const std::string &command) {
        CT->CoreCommand = command;
        CT->SetProp(EProperty::CORE_COMMAND);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        std::string command;
        Get(command);
        spec.set_core_command(command);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_core_command();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.core_command());
    }
} static CoreCommand;

class TVirtMode : public TProperty {
public:
    TVirtMode() : TProperty(P_VIRT_MODE, EProperty::VIRT_MODE,
            "Virtualization mode: os|app|job|host") {}
    TError Get(std::string &value) {
        value = CT->OsMode ? "os" :
                CT->JobMode ? "job" :
                CT->HostMode ? "host" : "app";
        return OK;
    }
    TError Set(const std::string &value) {

        if (value != "app" &&
                value != "os" &&
                value != "job" &&
                value != "host")
            return TError(EError::InvalidValue, "Unknown: {}", value);

        CT->OsMode = false;
        CT->JobMode = false;
        CT->HostMode = false;

        if (value == "os")
            CT->OsMode = true;
        else if (value == "job")
            CT->JobMode = true;
        else if (value == "host")
            CT->HostMode = true;

        if (CT->HostMode || CT->JobMode)
            CT->Isolate = false;

        CT->SetProp(EProperty::VIRT_MODE);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        std::string val;
        Get(val);
        spec.set_virt_mode(val);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_virt_mode();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.virt_mode());
    }
} static VirtMode;

class TStdinPath : public TProperty {
public:
    TStdinPath() : TProperty(P_STDIN_PATH, EProperty::STDIN,
            "Container standard input path") {}
    TError Get(std::string &value) {
        value = CT->Stdin.Path.ToString();
        return OK;
    }
    TError Set(const std::string &path) {
        CT->Stdin.SetInside(path);
        CT->SetProp(EProperty::STDIN);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_stdin_path(CT->Stdin.Path.ToString());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_stdin_path();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.stdin_path());
    }
} static StdinPath;

class TStdoutPath : public TProperty {
public:
    TStdoutPath() : TProperty(P_STDOUT_PATH, EProperty::STDOUT,
            "Container standard output path") {}
    TError Get(std::string &value) {
        value =  CT->Stdout.Path.ToString();
        return OK;
    }
    TError Set(const std::string &path) {
        CT->Stdout.SetInside(path);
        CT->SetProp(EProperty::STDOUT);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::STDOUT))
            CT->Stdout.SetOutside("/dev/null");
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_stdout_path(CT->Stdout.Path.ToString());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_stdout_path();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.stdout_path());
    }
} static StdoutPath;

class TStderrPath : public TProperty {
public:
    TStderrPath() : TProperty(P_STDERR_PATH, EProperty::STDERR,
            "Container standard error path") {}
    TError Get(std::string &value) {
        value = CT->Stderr.Path.ToString();
        return OK;
    }
    TError Set(const std::string &path) {
        CT->Stderr.SetInside(path);
        CT->SetProp(EProperty::STDERR);
        return OK;
    }
    TError Start(void) {
         if (CT->OsMode && !CT->HasProp(EProperty::STDERR))
            CT->Stderr.SetOutside("/dev/null");
         return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
       spec.set_stderr_path(CT->Stderr.Path.ToString());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_stderr_path();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.stderr_path());
    }
} static StderrPath;

class TStdoutLimit : public TProperty {
public:
    TStdoutLimit() : TProperty(P_STDOUT_LIMIT, EProperty::STDOUT_LIMIT,
            "Limit for stored stdout and stderr size")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->Stdout.Limit);
        return OK;
    }

    TError Set(uint64_t limit) {
        uint64_t limit_max = config().container().stdout_limit_max();
        if (limit > limit_max && !CL->IsSuperUser())
            return TError(EError::Permission,
                          "Maximum limit is: " + std::to_string(limit_max));

        CT->Stdout.Limit = limit;
        CT->Stderr.Limit = limit;
        CT->SetProp(EProperty::STDOUT_LIMIT);
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t limit;
        TError error = StringToSize(value, limit);
        if (error)
            return error;

        return Set(limit);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_stdout_limit(CT->Stdout.Limit);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_stdout_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.stdout_limit());
    }
} static StdoutLimit;

class TStdoutOffset : public TProperty {
public:
    TStdoutOffset() : TProperty(P_STDOUT_OFFSET, EProperty::NONE,
            "Offset of stored stdout")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->Stdout.Offset);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_stdout_offset(CT->Stdout.Offset);
    }
} static StdoutOffset;

class TStderrOffset : public TProperty {
public:
    TStderrOffset() : TProperty(P_STDERR_OFFSET, EProperty::NONE,
            "Offset of stored stderr")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->Stderr.Offset);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_stderr_offset(CT->Stderr.Offset);
    }
} static StderrOffset;

class TStdout : public TProperty {
public:
    TStdout() : TProperty(P_STDOUT, EProperty::NONE,
            "Read stdout [[offset][:length]]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }
    TError Get(std::string &value) {
        return CT->Stdout.Read(*CT, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        return CT->Stdout.Read(*CT, value, index);
    }

    void DumpIndexed(const std::string &index, rpc::TContainerStatus &spec) override {
        std::string value;
        auto error = GetIndexed(index, value);
        if (!error)
            spec.set_stdout(value);
    }
} static Stdout;

class TStderr : public TProperty {
public:
    TStderr() : TProperty(P_STDERR, EProperty::NONE,
            "Read stderr [[offset][:length]])")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }
    TError Get(std::string &value) {
        return CT->Stderr.Read(*CT, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        return CT->Stderr.Read(*CT, value, index);
    }

    void DumpIndexed(const std::string &index, rpc::TContainerStatus &spec) override {
        std::string value;
        auto error = GetIndexed(index, value);
        if (!error)
            spec.set_stderr(value);
    }
} static Stderr;

class TBindDns : public TProperty {
public:
    TBindDns() : TProperty(P_BIND_DNS, EProperty::BIND_DNS,
            "Bind /etc/hosts from parent, deprecated")
    {
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->BindDns);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = StringToBool(value, CT->BindDns);
        if (error)
            return error;
        CT->SetProp(EProperty::BIND_DNS);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::BIND_DNS))
            CT->BindDns = false;
        return OK;
    }
} static BindDns;

class TIsolate : public TProperty {
public:
    TIsolate() : TProperty(P_ISOLATE, EProperty::ISOLATE,
            "New pid/ipc/utc/env namespace") {}
    TError Get(std::string &value) {
        value = BoolToString(CT->Isolate);
        return OK;
    }

    TError Set(bool value) {
        if (value && (CT->HostMode || CT->JobMode))
            return TError(EError::InvalidValue, "isolate=true incompatible with virt_mode");
        CT->Isolate = value;
        CT->SetProp(EProperty::ISOLATE);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_isolate(CT->Isolate);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_isolate();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.isolate());
    }
} static Isolate;

class TRoot : public TProperty {
public:
    TRoot() : TProperty(P_ROOT, EProperty::ROOT, "Container root path in parent namespace") {}
    TError Get(std::string &value) {
        value = CT->Root;
        return OK;
    }
    TError Set(const std::string &value) {
        TError error;

        if (CT->VolumeMounts)
            return TError(EError::Busy, "Cannot change root path, container have volume mounts");

        error = CT->EnableControllers(CGROUP_DEVICES);
        if (error)
            return error;

        if (TPath(value).NormalPath().StartsWithDotDot())
            return TError(EError::Permission, "root path starts with ..");

        CT->Root = value;
        CT->SetProp(EProperty::ROOT);
        CT->TaintFlags.RootOnLoop = false;
        CT->SanitizeCapabilitiesAll();

        auto subtree = CT->Subtree();
        subtree.reverse();
        for (auto &ct: subtree)
            ct->RootPath = ct->Parent->RootPath / TPath(ct->Root).NormalPath();

        return OK;
    }
    TError Start(void) {
        if ((CT->HostMode || CT->JobMode) && CT->Root != "/")
            return TError(EError::InvalidValue, "Cannot change root in this virt_mode");
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_root(CT->Root);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_root();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.root());
    }
} static Root;

class TRootPath : public TProperty {
public:
    TRootPath() : TProperty(P_ROOT_PATH, EProperty::NONE, "Container root path in client namespace") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = CL->ComposePath(CT->RootPath).ToString();
        if (value == "")
            return TError(EError::Permission, "Root path is unreachable");
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        TPath root = CL->ComposePath(CT->RootPath);
        if (root)
            spec.set_root_path(root.ToString());
    }
} static RootPath;

class TNet : public TProperty {
public:
    TNet() : TProperty(P_NET, EProperty::NET,
            "Container network settings: "
            "none | "
            "inherited (default) | "
            "steal <name> | "
            "container <name> | "
            "macvlan <master> <name> [bridge|private|vepa|passthru] [mtu] [hw] | "
            "ipvlan <master> <name> [l2|l3] [mtu] | "
            "veth <name> <bridge> [mtu] [hw] | "
            "L3 <name> [master] | "
            "NAT [name] | "
            "ipip6 <name> <remote> <local> | "
            "tap <name> | "
            "MTU <name> <mtu> | "
            "MAC <name> <mac> | "
            "autoconf <name> (SLAAC) | "
            "ip <cmd> <args>... | "
            "netns <name>") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->NetProp, ' ', ';');
        return OK;
    }

    TError Set(TMultiTuple &val) {
        TNetEnv NetEnv;
        TError error = NetEnv.ParseNet(val);
        if (error)
            return error;
        if (!NetEnv.NetInherit && !NetEnv.NetNone) {
            error = CT->EnableControllers(CGROUP_NETCLS);
            if (error)
                return error;
        }
        CT->NetProp = val; /* FIXME: Copy vector contents? */
        CT->NetIsolate = NetEnv.NetIsolate;
        CT->NetInherit = NetEnv.NetInherit;
        CT->SetProp(EProperty::NET);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }

    TError Set(const std::string &net_desc) {
        auto new_net_desc = SplitEscapedString(net_desc, ' ', ';');
        return Set(new_net_desc);
    }
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::NET)) {
            CT->NetProp = { { "none" } };
            CT->NetIsolate = true;
            CT->NetInherit = false;
        }
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_net();
        for (auto &line: CT->NetProp) {
            auto cfg = out->add_cfg();
            cfg->set_opt(line[0]);
            bool first = true;
            for (auto &word: line) {
                if (!first)
                    cfg->add_arg(word);
                first = false;
            }
            out->set_inherited(CT->NetInherit);
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_net();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TMultiTuple net;
        for (auto &cfg: spec.net().cfg()) {
            net.push_back({cfg.opt()});
            for (auto &arg: cfg.arg())
                net.back().push_back(arg);
        }
        return Set(net);
    }
} static Net;

class TRootRo : public TProperty {
public:
    TRootRo() : TProperty(P_ROOT_RDONLY, EProperty::ROOT_RDONLY,
            "Make filesystem read-only") {}
    TError Get(std::string &value) {
        value = BoolToString(CT->RootRo);
        return OK;
    }

    TError Set(bool value) {
        CT->RootRo = value;
        CT->SetProp(EProperty::ROOT_RDONLY);
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_root_readonly(CT->RootRo);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_root_readonly();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.root_readonly());
    }
} static RootRo;

class TUmask : public TProperty {
public:
    TUmask() : TProperty(P_UMASK, EProperty::UMASK,
            "Set file mode creation mask") { }
    TError Get(std::string &value) {
        value = StringFormat("%#o", CT->Umask);
        return OK;
    }

    TError Set(unsigned value) {
        CT->Umask = value;
        CT->SetProp(EProperty::UMASK);
        return OK;
    }

    TError Set(const std::string &value) {
        unsigned val;
        TError error = StringToOct(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        std::string val;
        if (!Get(val))
            spec.set_umask(std::stoi(val));
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_umask();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        unsigned val;
        auto error = StringToOct(std::to_string(spec.umask()), val);
        if (error)
            return error;
        return Set(val);
    }
} static Umask;

class TControllers : public TProperty {
public:
    TControllers() : TProperty(P_CONTROLLERS, EProperty::CONTROLLERS,
            "Cgroup controllers") { }
    TError Get(std::string &value) {
        // TODO: backward compatibility, remove when porto with perf_event cgroup support will be on the whole cluster
        uint64_t controllers = CT->Controllers & ~CGROUP_PERF;
        value = StringFormatFlags(controllers, ControllersName, ";");
        return OK;
    }
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringParseFlags(value, ControllersName, val, ';');
        if (error)
            return error;
        if ((val & CT->RequiredControllers) != CT->RequiredControllers)
            return TError(EError::InvalidValue, "Cannot disable required controllers");
        CT->Controllers = val;
        CT->SetProp(EProperty::CONTROLLERS);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        // TODO: backward compatibility, remove when porto with perf_event cgroup support will be on the whole cluster
        uint64_t controllers = CT->Controllers & ~CGROUP_PERF;
        uint64_t val;
        TError error = StringParseFlags(index, ControllersName, val, ';');
        if (error)
            return error;
        value = BoolToString((controllers & val) == val);
        return OK;
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        uint64_t val;
        bool enable;
        TError error = StringParseFlags(index, ControllersName, val, ';');
        if (error)
            return error;
        error = StringToBool(value, enable);
        if (error)
            return error;
        if (enable)
            val = CT->Controllers | val;
        else
            val = CT->Controllers & ~val;
        if ((val & CT->RequiredControllers) != CT->RequiredControllers)
            return TError(EError::InvalidValue, "Cannot disable required controllers");
        CT->Controllers = val;
        CT->SetProp(EProperty::CONTROLLERS);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_controllers();
        // TODO: backward compatibility, remove when porto with perf_event cgroup support will be on the whole cluster
        uint64_t controllers = CT->Controllers & ~CGROUP_PERF;
        for (auto &it: ControllersName)
            if (controllers & it.first)
                out->add_controller(it.second);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_controllers();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        uint64_t controllers = 0, val;
        for (auto &name: spec.controllers().controller()) {
            TError error = StringParseFlags(name, ControllersName, val, ';');
            if (error)
                return error;
            controllers |= val;
        }
        if ((controllers & CT->RequiredControllers) != CT->RequiredControllers)
            return TError(EError::InvalidValue, "Cannot disable required controllers");
        CT->Controllers = controllers;
        CT->SetProp(EProperty::CONTROLLERS);
        return OK;
    }
} static Controllers;

class TCgroups : public TProperty {
public:
    TCgroups() : TProperty(P_CGROUPS, EProperty::NONE, "Cgroups") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TStringMap map;
        for (auto &subsys: Subsystems)
            map[subsys->Type] = CT->GetCgroup(*subsys).Path().ToString();
        value = StringMapToString(map);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        for (auto &subsys: Subsystems) {
            if (subsys->Type != index)
                continue;
            value = CT->GetCgroup(*subsys).Path().ToString();
            return OK;
        }
        return TError(EError::InvalidProperty, "Unknown cgroup subststem: " + index);
    }

    void Dump(rpc::TContainerStatus &spec) override {
        auto out = spec.mutable_cgroups();
        for (auto &subsys: Subsystems) {
            auto cg = out->add_cgroup();
            cg->set_controller(subsys->Type);
            cg->set_path(CT->GetCgroup(*subsys).Path().ToString());
            if (!(CT->Controllers & subsys->Controllers))
                cg->set_inherited(true);
        }
    }
} static Cgroups;

class THostname : public TProperty {
public:
    THostname() : TProperty(P_HOSTNAME, EProperty::HOSTNAME,
            "Container hostname") {}
    TError Get(std::string &value) {
        value = CT->Hostname;
        return OK;
    }
    TError Set(const std::string &hostname) {
        CT->Hostname = hostname;
        CT->SetProp(EProperty::HOSTNAME);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_hostname(CT->Hostname);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_hostname();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.hostname());
    }
} static Hostname;

class TEnvProperty : public TProperty {
public:
    TEnvProperty() : TProperty(P_ENV, EProperty::ENV,
            "Container environment variables: <name>=<value>; ...") {}
    TError Get(std::string &val) {
        val = CT->EnvCfg;
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TEnv env;
        TError error = CT->GetEnvironment(env);
        if (error)
            return error;
        return env.GetEnv(index, value);
    }
    TError Set(const std::string &val) {
        TEnv env;
        TError error =  env.Parse(val, true);
        if (error)
            return error;
        env.Format(CT->EnvCfg);
        CT->SetProp(EProperty::ENV);
        return OK;
    }
    TError SetIndexed(const std::string &index, const std::string &val) {
        TEnv env;
        TError error = env.Parse(CT->EnvCfg, true);
        if (error)
            return error;
        error = env.SetEnv(index, val);
        if (error)
            return error;
        env.Format(CT->EnvCfg);
        CT->SetProp(EProperty::ENV);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        TEnv env;
        if (CT->GetEnvironment(env))
            return;
        auto e = spec.mutable_env();
        for (auto &var: env.Vars) {
            if (var.Overwritten) {
                auto v = e->add_var();
                v->set_name(var.Name);
                if (var.Set)
                    v->set_value(var.Secret ? "<secret>" : var.Value);
                else
                    v->set_unset(true);
            }
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_env();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TEnv env;
        TError error;

        if (spec.env().merge()) {
            error = env.Parse(CT->EnvCfg, true);
            if (error)
                return error;
        }
        for (auto &var: spec.env().var()) {
            if (var.has_value())
                error = env.SetEnv(var.name(), var.value());
            else
                error = env.UnsetEnv(var.name());
            if (error)
                return error;
        }
        env.Format(CT->EnvCfg);
        CT->SetProp(EProperty::ENV);
        return OK;
    }
} static EnvProperty;

class TEnvSecretProperty : public TProperty {
public:
    TEnvSecretProperty() : TProperty(P_ENV_SECRET, EProperty::ENV_SECRET,
            "Container secret environment variables: <name>=<value>; ...") {}
    TError Save(std::string &val) override {
        val = CT->EnvSecret;
        return OK;
    }
    TError Get(std::string &val) {
        TEnv env;
        TError error = env.Parse(CT->EnvSecret, true, true);
        if (error)
            return error;
        env.Format(val);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TEnv env;
        TError error = env.Parse(CT->EnvSecret, true, true);
        if (error)
            return error;
        return env.GetEnv(index, value);
    }
    TError Set(const std::string &val) {
        TEnv env;
        TError error = env.Parse(val, true, true);
        if (error)
            return error;
        env.Format(CT->EnvSecret, true);
        CT->SetProp(EProperty::ENV_SECRET);
        return OK;
    }
    TError SetIndexed(const std::string &index, const std::string &val) {
        TEnv env;
        TError error = env.Parse(CT->EnvSecret, true, true);
        if (error)
            return error;
        error = env.SetEnv(index, val, true, false, true);
        if (error)
            return error;
        env.Format(CT->EnvSecret, true);
        CT->SetProp(EProperty::ENV_SECRET);
        return OK;
    }
    void Dump(rpc::TContainerSpec &spec) {
        TEnv env;
        if (env.Parse(CT->EnvSecret, true, true))
            return;
        auto e = spec.mutable_env_secret();
        for (auto &var: env.Vars) {
            auto v = e->add_var();
            v->set_name(var.Name);
            if (var.Set)
                v->set_value("<secret>");
            else
                v->set_unset(true);
        }
    }
    bool Has(const rpc::TContainerSpec &spec) {
        return spec.has_env_secret();
    }
    TError Load(const rpc::TContainerSpec &spec) {
        TEnv env;
        TError error;

        if (spec.env_secret().merge()) {
            error = env.Parse(CT->EnvSecret, true, true);
            if (error)
                return error;
        }
        for (auto &var: spec.env_secret().var()) {
            if (var.has_value())
                error = env.SetEnv(var.name(), var.value(), true, false, true);
            else
                error = env.UnsetEnv(var.name());
            if (error)
                return error;
        }
        env.Format(CT->EnvSecret, true);
        CT->SetProp(EProperty::ENV_SECRET);
        return OK;
    }
} static EnvSecretProperty;

class TBind : public TProperty {
public:
    TBind() : TProperty(P_BIND, EProperty::BIND,
            "Bind mounts: <source> <target> [ro|rw|<flag>],... ;...") {}
    TError Get(std::string &value) {
        value = TBindMount::Format(CT->BindMounts);
        return OK;
    }
    TError Set(const std::string &value) {
        std::vector<TBindMount> result;
        TError error = TBindMount::Parse(value, result);
        if (error)
            return error;
        CT->BindMounts = result;
        CT->SetProp(EProperty::BIND);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_bind();
        for (auto &bind: CT->BindMounts)
            bind.Dump(*out->add_bind());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_bind();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        std::vector<TBindMount> result;
        result.resize(spec.bind().bind_size());
        for (int i = 0; i < spec.bind().bind_size(); i++) {
            TError error = result[i].Load(spec.bind().bind(i));
            if (error)
                return error;
        }
        CT->BindMounts = result;
        CT->SetProp(EProperty::BIND);
        return OK;
    }
} static Bind;

class TSymlink : public TProperty {
public:
    TSymlink() : TProperty(P_SYMLINK, EProperty::SYMLINK, "Symlinks: <symlink>: <target>;...")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        for (auto &link: CT->Symlink)
            value += fmt::format("{}: {}; ", link.first, link.second);
        return OK;
    }
    TError GetIndexed(const std::string &key, std::string &value) {
        TPath sym = TPath(key).NormalPath();
        auto it = CT->Symlink.find(sym);
        if (it == CT->Symlink.end())
            return TError(EError::NoValue, "Symlink {} not set", key);
        value = it->second.ToString();
        return OK;
    }
    TError SetIndexed(const std::string &key, const std::string &value) {
        auto sym = TPath(key).NormalPath();
        auto tgt = TPath(value).NormalPath();
        return CT->SetSymlink(sym, tgt);
    }
    TError Set(const std::string &value) {
        TStringMap map;
        TError error = StringToStringMap(value, map);
        if (error)
            return error;
        std::map<TPath, TPath> symlink;
        for (auto &link: map) {
            auto sym = TPath(link.first).NormalPath();
            auto tgt = TPath(link.second).NormalPath();
            symlink[sym] = tgt;
        }
        for (auto &link: CT->Symlink) {
            if (!symlink.count(link.first))
                symlink[link.first] = "";
        }
        for (auto &link: symlink) {
            error = CT->SetSymlink(link.first, link.second);
            if (error)
                return error;
        }
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_symlink();
        for (auto &link: CT->Symlink) {
            auto sym = out->add_map();
            sym->set_key(link.first.ToString());
            sym->set_val(link.second.ToString());
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_symlink();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TStringMap map;
        TError error;

        for (auto &sym: spec.symlink().map())
            map[sym.key()] = sym.val();

        if (!spec.symlink().merge())
            for (auto &sym: CT->Symlink)
                if (!map.count(sym.first.ToString()))
                        map[sym.first.ToString()] = "";

        for (auto &sym: map) {
            error = CT->SetSymlink(sym.first, sym.second);
            if (error)
                return error;
        }

        return OK;
    }
} static Symlink;

class TIp : public TProperty {
public:
    TIp() : TProperty(P_IP, EProperty::IP,
            "IP configuration: <interface> <ip>/<prefix>; ...") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->IpList, ' ', ';');
        return OK;
    }

    TError Set(TMultiTuple &ipaddrs) {
        TNetEnv NetEnv;
        TError error = NetEnv.ParseIp(ipaddrs);
        if (error)
            return error;
        CT->IpList = ipaddrs;
        CT->SetProp(EProperty::IP);
        return OK;
    }

    TError Set(const std::string &ipaddr) {
        auto ipaddrs = SplitEscapedString(ipaddr, ' ', ';');
        return Set(ipaddrs);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_ip();
        for (auto &line: CT->IpList) {
            auto ip = out->add_cfg();
            ip->set_dev(line[0]);
            ip->set_ip(line[1]);
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_ip();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TMultiTuple cfg;
        for (auto &line: spec.ip().cfg())
            cfg.push_back({line.dev(), line.ip()});
        return Set(cfg);
    }
} static Ip;

class TIpLimit : public TProperty {
public:
    TIpLimit() : TProperty(P_IP_LIMIT, EProperty::IP_LIMIT,
            "IP allowed for sub-containers: none|any|<ip>[/<mask>]; ...") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->IpLimit, ';', ' ');
        return OK;
    }

    TError Set(TMultiTuple &cfg) {
        TError error;
 
        if (cfg.empty())
            CT->IpPolicy = "any";

        for (auto &line: cfg) {
            if (line.size() != 1)
                return TError(EError::InvalidValue, "wrong format");
            if (line[0] == "any" || line[0] == "none") {
                if (cfg.size() != 1)
                    return TError(EError::InvalidValue, "more than one ip policy");
                CT->IpPolicy = line[0];
                continue;
            } else
                CT->IpPolicy = "some";

            TNlAddr addr;
            error = addr.Parse(AF_UNSPEC, line[0]);
            if (error)
                return error;
            if (addr.Family() != AF_INET && addr.Family() != AF_INET6)
                return TError(EError::InvalidValue, "wrong address");
        }

        CT->IpLimit = cfg;
        CT->SetProp(EProperty::IP_LIMIT);
        CT->SanitizeCapabilitiesAll();

        return OK;
    }

    TError Set(const std::string &value) {
        auto cfg = SplitEscapedString(value, ';', ' ');
        return Set(cfg);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto cfg = spec.mutable_ip_limit();
        cfg->set_policy(CT->IpPolicy);
        if (CT->IpPolicy == "some")
            for (auto &line: CT->IpLimit)
                cfg->add_ip(line[0]);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_ip_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TMultiTuple cfg;
        for (auto &ip: spec.ip_limit().ip())
            cfg.push_back({ip});
        if (cfg.empty())
            cfg.push_back({spec.ip_limit().policy()});
        return Set(cfg);
    }
} static IpLimit;

class TDefaultGw : public TProperty {
public:
    TDefaultGw() : TProperty(P_DEFAULT_GW, EProperty::DEFAULT_GW,
            "Default gateway: <interface> <ip>; ...") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->DefaultGw, ' ', ';');
        return OK;
    }

    TError Set(TMultiTuple &gws) {
        TNetEnv NetEnv;
        TError error = NetEnv.ParseGw(gws);
        if (error)
            return error;
        CT->DefaultGw = gws;
        CT->SetProp(EProperty::DEFAULT_GW);
        return OK;
    }

    TError Set(const std::string &gw) {
        auto gws = SplitEscapedString(gw, ' ', ';');
        return Set(gws);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_default_gw();
        for (auto &line: CT->DefaultGw) {
            auto ip = out->add_cfg();
            ip->set_dev(line[0]);
            ip->set_ip(line[1]);
        }
    }
    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_default_gw();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TMultiTuple cfg;
        for (auto &line: spec.default_gw().cfg())
            cfg.push_back({line.dev(), line.ip()});
        return Set(cfg);
    }
} static DefaultGw;

class TResolvConf : public TProperty {
public:
    TResolvConf() : TProperty(P_RESOLV_CONF, EProperty::RESOLV_CONF,
            "DNS resolver configuration: default|keep|<resolv.conf option>;...")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        if (CT->ResolvConf.size() || CT->IsRoot())
            value = StringReplaceAll(CT->ResolvConf, "\n", ";");
        else if (CT->HasProp(EProperty::RESOLV_CONF))
            value = "keep";
        else if (CT->Root == "/")
            value = "inherit";
        else
            value = "default";
        return OK;
    }
    TError Set(const std::string &value) {
        if (CT->State != EContainerState::Stopped &&
            ((CT->HasProp(EProperty::RESOLV_CONF) ? !CT->ResolvConf.size() : CT->Root == "/") !=
             (value == "keep" || value == "" || (CT->Root == "/" && value == "inherit"))))
            return TError(EError::InvalidState, "Cannot enable/disable resolv.conf overriding in runtime");
        if (value == "default" || value == "inherit") {
            CT->ResolvConf.clear();
            CT->ClearProp(EProperty::RESOLV_CONF);
        } else if (value == "keep" || value == "") {
            CT->ResolvConf.clear();
            CT->SetProp(EProperty::RESOLV_CONF);
        } else {
            CT->ResolvConf = StringReplaceAll(value, ";", "\n");
            CT->SetProp(EProperty::RESOLV_CONF);
        }
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        if (CT->ResolvConf.size() || CT->IsRoot())
            spec.set_resolv_conf(CT->ResolvConf);
        else if (CT->HasProp(EProperty::RESOLV_CONF))
            spec.set_resolv_conf("keep");
        else if (CT->Root == "/")
            spec.set_resolv_conf("inherit");
        else
            spec.set_resolv_conf("default");
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_resolv_conf();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.resolv_conf());
    }
} static ResolvConf;

class TEtcHosts : public TProperty {
public:
    TEtcHosts() : TProperty(P_ETC_HOSTS, EProperty::ETC_HOSTS, "Override /etc/hosts content")
    {
    }
    TError Get(std::string &value) {
        value = CT->EtcHosts;
        return OK;
    }
    TError Set(const std::string &value) {
        CT->EtcHosts = value;
        CT->SetProp(EProperty::ETC_HOSTS);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_etc_hosts(CT->EtcHosts);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_etc_hosts();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.etc_hosts());
    }
} static EtcHosts;

class TDevicesProperty : public TProperty {
public:
    TDevicesProperty() : TProperty(P_DEVICES, EProperty::DEVICE_CONF,
            "Devices that container can access: <device> [r][w][m][-][?] [path] [mode] [user] [group]|preset <preset>; ...")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = CT->Devices.Format();
        return OK;
    }
    TError Set(const std::string &value) {
        TDevices devices;

        // reset to default + extra + parent devices if empty string is given by user
        if (value.empty()) {
            CT->Devices.Devices.clear();
        } else {
            TError error = devices.Parse(value, CL->Cred);
            if (error)
                return error;

            if (devices.NeedCgroup) {
                error = CT->EnableControllers(CGROUP_DEVICES);
                if (error)
                    return error;
            }

            CT->Devices.Merge(devices, true, true);
        }

        CT->SetProp(EProperty::DEVICE_CONF);
        return OK;
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        TDevices devices;
        TError error = devices.Parse(index + " " + value, CL->Cred);
        if (error)
            return error;
        if (devices.NeedCgroup) {
            error = CT->EnableControllers(CGROUP_DEVICES);
            if (error)
                return error;
        }
        CT->Devices.Merge(devices, true);
        CT->SetProp(EProperty::DEVICE_CONF);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_devices();
        for (auto dev: CT->Devices.Devices)
            dev.Dump(*out->add_device());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_devices();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TDevices devices;
        TError error;
        error = CT->EnableControllers(CGROUP_DEVICES);
        if (error)
            return error;
        devices.NeedCgroup = true;
        devices.Devices.resize(spec.devices().device_size());
        for (int i = 0; i < spec.devices().device_size(); i++) {
            error = devices.Devices[i].Load(spec.devices().device(i), CL->Cred);
            if (error)
                return error;
        }
        CT->Devices.Merge(devices, true, !spec.devices().merge());
        CT->SetProp(EProperty::DEVICE_CONF);
        return OK;
    }
} static Devices;

class TRawRootPid : public TProperty {
public:
    TRawRootPid() : TProperty(P_RAW_ROOT_PID, EProperty::ROOT_PID, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = StringFormat("%d;%d;%d", CT->Task.Pid,
                                         CT->TaskVPid,
                                         CT->WaitTask.Pid);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error;

        auto val = SplitEscapedString(value, ';');
        if (val.size() > 0)
            error = StringToInt(val[0], CT->Task.Pid);
        else
            CT->Task.Pid = 0;
        if (!error && val.size() > 1)
            error = StringToInt(val[1], CT->TaskVPid);
        else
            CT->TaskVPid = 0;
        if (!error && val.size() > 2)
            error = StringToInt(val[2], CT->WaitTask.Pid);
        else
            CT->WaitTask.Pid = CT->Task.Pid;
        return error;
    }
} static RawRootPid;

class TSeizePid : public TProperty {
public:
    TSeizePid() : TProperty(P_SEIZE_PID, EProperty::SEIZE_PID, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->SeizeTask.Pid);
        return OK;
    }
    TError Set(const std::string &value) {
        return StringToInt(value, CT->SeizeTask.Pid);
    }
} static SeizePid;

class TRawCreationTime : public TProperty {
public:
    TRawCreationTime() : TProperty(P_RAW_CREATION_TIME, EProperty::CREATION_TIME, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->CreationTime);
        return OK;
    }
    TError Set(const std::string &value) {
        auto error = StringToUint64(value, CT->CreationTime);
        CT->RealCreationTime = time(nullptr) - (GetCurrentTimeMs() - CT->CreationTime) / 1000;
        return OK;
    }
} static RawCreationTime;

class TRawStartTime : public TProperty {
public:
    TRawStartTime() : TProperty(P_RAW_START_TIME, EProperty::START_TIME, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->StartTime);
        return OK;
    }
    TError Set(const std::string &value) {
        StringToUint64(value, CT->StartTime);
        CT->RealStartTime = time(nullptr) - (GetCurrentTimeMs() - CT->StartTime) / 1000;
        return OK;
    }
} static RawStartTime;

class TRawDeathTime : public TProperty {
public:
    TRawDeathTime() : TProperty(P_RAW_DEATH_TIME, EProperty::DEATH_TIME, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->DeathTime);
        return OK;
    }
    TError Set(const std::string &value) {
        StringToUint64(value, CT->DeathTime);
        CT->RealDeathTime = time(nullptr) - (GetCurrentTimeMs() - CT->DeathTime) / 1000;
        return OK;
    }
} static RawDeathTime;

class TPortoNamespace : public TProperty {
public:
    TPortoNamespace() : TProperty(P_PORTO_NAMESPACE, EProperty::PORTO_NAMESPACE,
            "Porto containers namespace (container name prefix) (deprecated, use enable_porto=isolate instead)") {}
    TError Get(std::string &value) {
        value = CT->NsName;
        return OK;
    }
    TError Set(const std::string &value) {
        CT->NsName = value;
        CT->SetProp(EProperty::PORTO_NAMESPACE);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_porto_namespace(CT->NsName);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_porto_namespace();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.porto_namespace());
    }
} static PortoNamespace;

class TPlaceProperty : public TProperty {
public:
    TPlaceProperty() : TProperty(P_PLACE, EProperty::PLACE,
            "Places for volumes and layers: [default][;/path...][;***][;alias=/path]") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->PlacePolicy, ';');
        return OK;
    }
    TError Set(const std::string &value) {
        CT->PlacePolicy = SplitEscapedString(value, ';');
        CT->SetProp(EProperty::PLACE);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto cfg = spec.mutable_place();
        for (auto &place: CT->PlacePolicy) {
            auto p = cfg->add_cfg();
            auto sep = place.find('=');
            if (place[0] == '/' || place == "***" || sep == std::string::npos) {
                p->set_place(place);
            } else {
                p->set_place(place.substr(sep + 1));
                p->set_alias(place.substr(0, sep));
            }
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_place();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        CT->PlacePolicy.clear();
        for (auto &p: spec.place().cfg()) {
            if (p.has_alias())
                CT->PlacePolicy.push_back(p.alias() + "=" + p.place());
            else
                CT->PlacePolicy.push_back(p.place());
        }
        CT->SetProp(EProperty::PLACE);
        return OK;
    }
} static PlaceProperty;

class TPlaceLimit : public TProperty {
public:
    TPlaceLimit() : TProperty(P_PLACE_LIMIT, EProperty::PLACE_LIMIT,
            "Limits sum of volume space_limit: total|default|/place|tmpfs|lvm group|rbd: bytes;...")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        auto lock = LockVolumes();
        return UintMapToString(CT->PlaceLimit, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        auto lock = LockVolumes();
        if (!CT->PlaceLimit.count(index))
            return TError(EError::InvalidValue, "invalid index " + index);
        value = std::to_string(CT->PlaceLimit.at(index));
        return OK;
    }

    TError Set(TUintMap& value) {
        auto lock = LockVolumes();
        CT->PlaceLimit = value;
        CT->SetProp(EProperty::PLACE_LIMIT);
        return OK;
    }

    TError Set(const std::string &value) {
        TUintMap val;
        TError error = StringToUintMap(value, val);
        if (error)
            return error;
        return Set(val);
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        auto lock = LockVolumes();
        TUintMap val = CT->PlaceLimit;
        TError error = StringToSize(value, val[index]);
        if (!error) {
            CT->PlaceLimit = val;
            CT->SetProp(EProperty::PLACE_LIMIT);
        }
        return error;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        DumpMap(CT->PlaceLimit, *spec.mutable_place_limit());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_place_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TUintMap map;
        LoadMap(spec.place_limit(), CT->PlaceLimit, map);
        return Set(map);
    }
} static PlaceLimit;

class TPlaceUsage : public TProperty {
public:
    TPlaceUsage() : TProperty(P_PLACE_USAGE, EProperty::NONE,
            "Current sum of volume space_limit: total|/place|tmpfs|lvm group|rbd: bytes;...") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        auto lock = LockVolumes();
        return UintMapToString(CT->PlaceUsage, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        auto lock = LockVolumes();
        if (!CT->PlaceUsage.count(index))
            return TError(EError::InvalidValue, "invalid index " + index);
        value = std::to_string(CT->PlaceUsage.at(index));
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        DumpMap(CT->PlaceUsage, *spec.mutable_place_usage());
    }
} static PlaceUsage;

class TOwnedVolumes : public TProperty {
public:
    TOwnedVolumes() : TProperty(P_OWNED_VOLUMES, EProperty::NONE, "Owned volumes: volume;...")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TTuple paths;

        for (auto &vol: CT->OwnedVolumes) {
            TPath path = CL->ComposePath(vol->Path);
            if (!path)
                path = "@" + vol->Path.ToString();
            paths.push_back(path.ToString());
        }

        value = MergeEscapeStrings(paths, ';');
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        auto out = spec.mutable_volumes_owned();
        for (auto &vol: CT->OwnedVolumes) {
            TPath path = CL->ComposePath(vol->Path);
            if (!path)
                path = "@" + vol->Path.ToString();
            out->add_volume(path.ToString());
        }
    }
} OwnedVolumes;

class TLinkedVolumes : public TProperty {
public:
    TLinkedVolumes() : TProperty(P_LINKED_VOLUMES, EProperty::NONE, "Linked volumes: volume [target] [ro] [!];...")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TMultiTuple links;

        auto volumes_lock = LockVolumes();
        links.reserve(CT->VolumeLinks.size());

        for (auto &link: CT->VolumeLinks) {
            TPath path = link->Volume->ComposePathLocked(*CL->ClientContainer);
            if (!path)
                path = "%" + link->Volume->Path.ToString();
            links.push_back({path.ToString()});
            if (link->Target)
                links.back().push_back(link->Target.ToString());
            if (link->ReadOnly)
                links.back().push_back("ro");
            if (link->Required)
                links.back().push_back("!");
        }
        volumes_lock.unlock();

        value = MergeEscapeStrings(links, ' ', ';');
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        auto out = spec.mutable_volumes_linked();

        auto volumes_lock = LockVolumes();
        for (auto &link: CT->VolumeLinks) {
            auto l = out->add_link();

            TPath path = link->Volume->ComposePathLocked(*CL->ClientContainer);
            if (!path)
                path = "%" + link->Volume->Path.ToString();
            l->set_volume(path.ToString());
            if (link->Target)
                l->set_target(link->Target.ToString());
            if (link->ReadOnly)
                l->set_read_only(true);
            if (link->Required)
                l->set_required(true);
        }
        volumes_lock.unlock();
    }
} LinkedVolumes;

class TRequiredVolumes : public TProperty {
public:
    TRequiredVolumes() : TProperty(P_REQUIRED_VOLUMES, EProperty::REQUIRED_VOLUMES,
            "Volume links required by container: path;...")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->RequiredVolumes, ';');
        return OK;
    }
    TError Set(const std::string &value) {
        auto volumes_lock = LockVolumes();
        auto prev = CT->RequiredVolumes;
        CT->RequiredVolumes = SplitEscapedString(value, ';');
        if (CT->HasResources()) {
            volumes_lock.unlock();;
            TError error = TVolume::CheckRequired(*CT);
            if (error) {
                volumes_lock.lock();
                CT->RequiredVolumes = prev;
                return error;
            }
        }
        CT->SetProp(EProperty::REQUIRED_VOLUMES);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_volumes_required();
        for (auto &path: CT->RequiredVolumes)
            out->add_volume(path);
    }
} static RequiredVolumes;

class TMemoryLimit : public TProperty {
public:
    TMemoryLimit() : TProperty(P_MEM_LIMIT, EProperty::MEM_LIMIT,
            "Memory limit [bytes]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }

    TError Get(uint64_t &value) {
        if (!CT->Level)
            value = GetTotalMemory() - GetHugetlbMemory();
        else
            value = CT->MemLimit;
        return OK;
    }

    TError Get(std::string &value) {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            value = std::to_string(val);
        return OK;
    }

    TError Set(uint64_t new_size) {
        if (new_size && new_size < config().container().min_memory_limit())
            return TError(EError::InvalidValue, "Should be at least {}", config().container().min_memory_limit());
        if (CT->MemLimit != new_size) {
            CT->MemLimit = new_size;
            CT->SetProp(EProperty::MEM_LIMIT);
            CT->SanitizeCapabilitiesAll();
        }
        if (!CT->HasProp(EProperty::ANON_LIMIT) &&
                MemorySubsystem.SupportAnonLimit() &&
                config().container().anon_limit_margin()) {
            uint64_t new_anon = 0;
            if (CT->MemLimit) {
                new_anon = CT->MemLimit - std::min(CT->MemLimit / 4,
                        config().container().anon_limit_margin());
                new_anon = std::max(new_anon,
                        config().container().min_memory_limit());
            }
            if (CT->AnonMemLimit != new_anon) {
                CT->AnonMemLimit = new_anon;
                CT->SetPropDirty(EProperty::ANON_LIMIT);
            }
        }
        return OK;
    }

    TError Set(const std::string &limit) {
        uint64_t new_size = 0lu;
        TError error = StringToSize(limit, new_size);
        if (error)
            return error;
        return Set(new_size);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        uint64_t val;
        if (!Get(val))
            spec.set_memory_limit(val);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_memory_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.memory_limit());
    }
} static MemoryLimit;

class TMemoryLimitTotal : public TProperty {
public:
    TMemoryLimitTotal() : TProperty(P_MEM_LIMIT_TOTAL, EProperty::NONE,
            "Effective memory limit [bytes]") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->GetMemLimit());
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_memory_limit_total(CT->GetMemLimit());
    }
} static MemoryLimitTotal;

class TAnonLimit : public TProperty {
public:
    TAnonLimit() : TProperty(P_ANON_LIMIT, EProperty::ANON_LIMIT,
            "Anonymous memory limit [bytes]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportAnonLimit();
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->AnonMemLimit);
        return OK;
    }

    TError Set(uint64_t new_size) {
        if (new_size && new_size < config().container().min_memory_limit())
            return TError(EError::InvalidValue, "Should be at least {}", config().container().min_memory_limit());
        if (CT->AnonMemLimit != new_size) {
            CT->AnonMemLimit = new_size;
            CT->SetProp(EProperty::ANON_LIMIT);
        }
        return OK;
    }

    TError Set(const std::string &limit) {
        uint64_t new_size;
        TError error = StringToSize(limit, new_size);
        if (error)
            return error;
        return Set(new_size);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_anon_limit(CT->AnonMemLimit);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_anon_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.anon_limit());
    }
} static AnonLimit;

class TAnonLimitTotal : public TProperty {
public:
    TAnonLimitTotal() : TProperty(P_ANON_LIMIT_TOTAL, EProperty::NONE,
            "Effective anonymous memory limit [bytes]") {
        IsReadOnly = true;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportAnonLimit();
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->GetAnonMemLimit());
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_anon_limit_total(CT->GetAnonMemLimit());
    }
} static AnonLimitTotal;

class TAnonOnly : public TProperty {
public:
    TAnonOnly() : TProperty(P_ANON_ONLY, EProperty::ANON_ONLY,
            "Keep only anon pages, allocate cache in parent")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportAnonOnly();
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->AnonOnly);
        return OK;
    }

    TError Set(bool value) {
        if (value != CT->AnonOnly) {
            CT->AnonOnly = value;
            CT->SetProp(EProperty::ANON_ONLY);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_anon_only(CT->AnonOnly);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_anon_only();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.anon_only());
    }
} static AnonOnly;

class TDirtyLimit : public TProperty {
public:
    TDirtyLimit() : TProperty(P_DIRTY_LIMIT, EProperty::DIRTY_LIMIT,
            "Dirty file cache limit [bytes]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsHidden = !MemorySubsystem.SupportDirtyLimit();
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->DirtyMemLimit);
        return OK;
    }

    TError Set(uint64_t new_size) {
        if (new_size && new_size < config().container().min_memory_limit())
            return TError(EError::InvalidValue, "Should be at least {}", config().container().min_memory_limit());
        if (CT->DirtyMemLimit != new_size) {
            CT->DirtyMemLimit = new_size;
            CT->SetProp(EProperty::DIRTY_LIMIT);
        }
        return OK;
    }

    TError Set(const std::string &limit) {
        uint64_t new_size;
        TError error = StringToSize(limit, new_size);
        if (error)
            return error;
        return Set(new_size);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_dirty_limit(CT->DirtyMemLimit);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_dirty_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.dirty_limit());
    }
} static DirtyLimit;

class THugetlbLimit : public TProperty {
public:
    THugetlbLimit() : TProperty(P_HUGETLB_LIMIT, EProperty::HUGETLB_LIMIT,
            "Hugetlb memory limit [bytes]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_HUGETLB;
    }
    void Init(void) {
        IsSupported = HugetlbSubsystem.Supported;
    }
    TError Get(std::string &value) {
        if (!CT->Level)
            value = std::to_string(GetHugetlbMemory());
        else
            value = std::to_string(CT->HugetlbLimit);
        return OK;
    }

    TError Set(uint64_t limit) {
        auto cg = CT->GetCgroup(HugetlbSubsystem);
        uint64_t usage;
        if (!HugetlbSubsystem.GetHugeUsage(cg, usage) && limit < usage)
            return TError(EError::InvalidValue, "current hugetlb usage is greater than limit");
        CT->HugetlbLimit = limit;
        CT->SetProp(EProperty::HUGETLB_LIMIT);
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t limit;
        auto error = StringToSize(value, limit);
        if (error)
            return error;
        return Set(limit);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_hugetlb_limit(CT->HugetlbLimit);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_hugetlb_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.hugetlb_limit());
    }
} static HugetlbLimit;

class TRechargeOnPgfault : public TProperty {
public:
    TRechargeOnPgfault() : TProperty(P_RECHARGE_ON_PGFAULT, EProperty::RECHARGE_ON_PGFAULT,
            "Recharge memory on page fault")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportRechargeOnPgfault();
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->RechargeOnPgfault);
        return OK;
    }

    TError Set(bool value) {
        if (value != CT->RechargeOnPgfault) {
            CT->RechargeOnPgfault = value;
            CT->SetProp(EProperty::RECHARGE_ON_PGFAULT);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_recharge_on_pgfault(CT->RechargeOnPgfault);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_recharge_on_pgfault();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.recharge_on_pgfault());
    }
} static RechargeOnPgfault;

class TPressurizeOnDeath : public TProperty {
public:
    TPressurizeOnDeath() : TProperty(P_PRESSURIZE_ON_DEATH, EProperty::PRESSURIZE_ON_DEATH,
            "After death set tiny soft memory limit")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->PressurizeOnDeath);
        return OK;
    }

    TError Set(bool value) {
        if (value != CT->PressurizeOnDeath) {
            CT->PressurizeOnDeath = value;
            CT->SetProp(EProperty::PRESSURIZE_ON_DEATH);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_pressurize_on_death(CT->PressurizeOnDeath);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_pressurize_on_death();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.pressurize_on_death());
    }
} static PressurizeOnDeath;

class TCpuLimit : public TProperty {
public:
    TCpuLimit() : TProperty(P_CPU_LIMIT, EProperty::CPU_LIMIT,
            "CPU limit: <CPUS>c [cores]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_CPU;
    }
    TError Get(std::string &value) {
        value = CpuPowerToString(CT->CpuLimit);
        return OK;
    }

    TError Set(double val) {
        uint64_t power = val * CPU_POWER_PER_SEC;
        if (CT->CpuLimit != power) {
            CT->CpuLimit = power;
            CT->SetProp(EProperty::CPU_LIMIT);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t limit;
        TError error = StringToCpuPower(value, limit);
        if (!error && CT->CpuLimit != limit) {
            CT->CpuLimit = limit;
            CT->SetProp(EProperty::CPU_LIMIT);
        }
        return error;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_cpu_limit((double)CT->CpuLimit / CPU_POWER_PER_SEC);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_cpu_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.cpu_limit());
    }
} static CpuLimit;

class TCpuLimitTotal : public TProperty {
public:
    TCpuLimitTotal() : TProperty(P_CPU_TOTAL_LIMIT, EProperty::NONE,
            "CPU total limit: <CPUS>c [cores]")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->CpuLimitSum)
            value = CpuPowerToString(CT->CpuLimitSum);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_cpu_limit_total((double)CT->CpuLimitSum / CPU_POWER_PER_SEC);
    }
} static CpuLimitTotal;

class TCpuGuarantee : public TProperty {
public:
    TCpuGuarantee() : TProperty(P_CPU_GUARANTEE, EProperty::CPU_GUARANTEE,
            "CPU guarantee: <CPUS>c [cores]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_CPU;
    }
    TError Get(std::string &value) {
        value = CpuPowerToString(CT->CpuGuarantee);
        return OK;
    }

    TError Set(double val) {
        uint64_t power = val * CPU_POWER_PER_SEC;
        if (CT->CpuGuarantee != power) {
            CT->CpuGuarantee = power;
            CT->SetProp(EProperty::CPU_GUARANTEE);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t guarantee;
        TError error = StringToCpuPower(value, guarantee);
        if (error)
            return error;
        if (CT->CpuGuarantee != guarantee) {
            CT->CpuGuarantee = guarantee;
            CT->SetProp(EProperty::CPU_GUARANTEE);
        }
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_cpu_guarantee((double)CT->CpuGuarantee / CPU_POWER_PER_SEC);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_cpu_guarantee();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.cpu_guarantee());
    }
} static CpuGuarantee;

class TCpuGuaranteeTotal : public TProperty {
public:
    TCpuGuaranteeTotal() : TProperty(P_CPU_TOTAL_GUARANTEE, EProperty::NONE,
            "CPU total guarantee: <CPUS>c [cores]")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->CpuGuarantee || CT->CpuGuaranteeSum)
            value = CpuPowerToString(std::max(CT->CpuGuarantee, CT->CpuGuaranteeSum));
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
         spec.set_cpu_guarantee_total((double)std::max(CT->CpuGuarantee, CT->CpuGuaranteeSum) / CPU_POWER_PER_SEC);
    }
} static CpuGuaranteeTotal;

class TCpuPeriod : public TProperty {
public:
    TCpuPeriod() : TProperty(P_CPU_PERIOD, EProperty::CPU_PERIOD,
            "CPU limit period: 1ms..1s, default: 100ms [nanoseconds]")
    {
        /* We want to allow changing period
           for the sake of incremental inheritancy */
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->CpuPeriod);
        return OK;
    }

    TError Set(uint64_t val) {
        if (val < 1000000 || val > 1000000000)
            return TError(EError::InvalidValue, "cpu period out of range");
        if (CT->CpuPeriod != val) {
            CT->CpuPeriod = val;
            CT->SetProp(EProperty::CPU_PERIOD);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToNsec(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_cpu_period(CT->CpuPeriod);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_cpu_period();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.cpu_period());
    }
} static CpuPeriod;

class TCpuWeight : public TProperty {
public:
    TCpuWeight() : TProperty(P_CPU_WEIGHT, EProperty::CPU_WEIGHT,
            "CPU weight 0.01..100, default is 1")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = StringFormat("%lg", CT->CpuWeight);
        return OK;
    }

    TError Set(double val) {
        if (val < 0.01 || val > 100)
            return TError(EError::InvalidValue, "out of range");

        if (CT->CpuWeight != val) {
            CT->CpuWeight = val;
            CT->SetProp(EProperty::CPU_WEIGHT);
            CT->ChooseSchedPolicy();
        }
        return OK;
    }

    TError Set(const std::string &value) {
        double val;
        std::string unit;
        TError error = StringToValue(value, val, unit);
        if (error || unit.size())
            return error;

        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_cpu_weight(CT->CpuWeight);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_cpu_weight();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.cpu_weight());
    }
} static CpuWeight;

class TCpuSet : public TProperty {
public:
    TCpuSet() : TProperty(P_CPU_SET, EProperty::CPU_SET,
            "CPU set: [N|N-M,]... | node N | reserve N | threads N | cores N")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_CPUSET;
    }
    TError Get(std::string &value) {
        auto lock = LockCpuAffinity();

        switch (CT->CpuSetType) {
        case ECpuSetType::Inherit:
            value = "";
            break;
        case ECpuSetType::Absolute:
            value = CT->CpuAffinity.Format();
            break;
        case ECpuSetType::Node:
            value = StringFormat("node %u", CT->CpuSetArg);
            break;
        case ECpuSetType::Reserve:
            value = StringFormat("reserve %u", CT->CpuSetArg);
            break;
        case ECpuSetType::Threads:
            value = StringFormat("threads %u", CT->CpuSetArg);
            break;
        case ECpuSetType::Cores:
            value = StringFormat("cores %u", CT->CpuSetArg);
            break;
        }

        if (!CT->CpuMems.empty()) {
            if (!value.empty())
                value += "; ";
            value += "mems " + CT->CpuMems;
        }

        return OK;
    }

    void SetMems(const std::string &mems) {
        if (CT->CpuMems != mems) {
            // FIXME don't forgot about childs and parents mems
            auto cg = CT->GetCgroup(CpusetSubsystem);
            auto error = CpusetSubsystem.SetMems(cg, mems);
            if (!error)
                CT->CpuMems = mems;
            else
                L_TAINT(fmt::format("Cannot set mems: {}", error));
        }
    }

    TError Set(const std::string &value) {
        auto cfgs = SplitEscapedString(value, ' ', ';');
        std::string mems;
        TTuple cfg;
        TError error;

        for (const auto& v : cfgs) {
            if (v.size() != 0 && v[0] != "mems")
                cfg = v;
            else if (v.size() == 2)
                mems = v[1];
        }

        auto lock = LockCpuAffinity();
        ECpuSetType type;
        int arg = !CT->CpuSetArg;

        if (cfg.size() == 0 || cfg[0] == "all" || cfg[0] == "inherit") {
            type = ECpuSetType::Inherit;
        } else if (cfg.size() == 1) {
            TBitMap map;
            error = map.Parse(cfg[0]);
            if (error)
                return error;
            type = ECpuSetType::Absolute;
            if (!CT->CpuAffinity.IsEqual(map)) {
                CT->CpuAffinity.Clear();
                CT->CpuAffinity.Set(map);
                CT->SetProp(EProperty::CPU_SET);
                CT->SetProp(EProperty::CPU_SET_AFFINITY);
            }
        } else if (cfg.size() == 2) {
            error = StringToInt(cfg[1], arg);
            if (error)
                return error;

            if (cfg[0] == "node")
                type = ECpuSetType::Node;
            else if (cfg[0] == "threads")
                type = ECpuSetType::Threads;
            else if (cfg[0] == "cores")
                type = ECpuSetType::Cores;
            else if (cfg[0] == "reserve")
                type = ECpuSetType::Reserve;
            else
                return TError(EError::InvalidValue, "wrong format");

            if (arg < 0 || (!arg && type != ECpuSetType::Node))
                return TError(EError::InvalidValue, "wrong format");

        } else
            return TError(EError::InvalidValue, "wrong format");

        SetMems(mems);

        if (CT->CpuSetType != type || CT->CpuSetArg != arg) {
            CT->CpuSetType = type;
            CT->CpuSetArg = arg;
            CT->SetProp(EProperty::CPU_SET);
        }

        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto cfg = spec.mutable_cpu_set();
        switch (CT->CpuSetType) {
        case ECpuSetType::Inherit:
            cfg->set_policy("inherit");
            break;
        case ECpuSetType::Absolute:
            cfg->set_policy("set");
            for (auto cpu = 0u; cpu < CT->CpuAffinity.Size(); cpu++)
                if (CT->CpuAffinity.Get(cpu))
                    cfg->add_cpu(cpu);
            cfg->set_list(CT->CpuAffinity.Format());
            cfg->set_count(CT->CpuAffinity.Weight());
            break;
        case ECpuSetType::Node:
            cfg->set_policy("node");
            cfg->set_arg(CT->CpuSetArg);
            break;
        case ECpuSetType::Reserve:
            cfg->set_policy("reserve");
            cfg->set_arg(CT->CpuSetArg);
            break;
        case ECpuSetType::Threads:
            cfg->set_policy("threads");
            cfg->set_arg(CT->CpuSetArg);
            break;
        case ECpuSetType::Cores:
            cfg->set_policy("cores");
            cfg->set_arg(CT->CpuSetArg);
            break;
        }
        cfg->set_mems(CT->CpuMems);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_cpu_set();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        auto cfg = spec.cpu_set();
        int arg = cfg.arg();
        ECpuSetType type;
        TError error;

        if (cfg.policy() == "inherit" || cfg.policy() == "") {
            type = ECpuSetType::Inherit;
        } else if (cfg.policy() == "set") {
            TBitMap map;

            if (cfg.has_list()) {
                error = map.Parse(cfg.list());
                if (error)
                    return error;
            } else {
                for (auto cpu: cfg.cpu())
                    map.Set(cpu);
            }

            type = ECpuSetType::Absolute;
            if (!CT->CpuAffinity.IsEqual(map)) {
                CT->CpuAffinity.Clear();
                CT->CpuAffinity.Set(map);
                CT->SetProp(EProperty::CPU_SET);
                CT->SetProp(EProperty::CPU_SET_AFFINITY);
            }
        } else if (cfg.policy() == "node") {
            type = ECpuSetType::Node;
        } else if (cfg.policy() == "threads") {
            type = ECpuSetType::Threads;
        } else if (cfg.policy() == "cores") {
            type = ECpuSetType::Cores;
        } else if (cfg.policy() == "reserve") {
            type = ECpuSetType::Reserve;
        } else
            return TError(EError::InvalidValue, "unknown cpu_set policy: {}", cfg.policy());

        if (CT->CpuSetType != type || CT->CpuSetArg != arg) {
            CT->CpuSetType = type;
            CT->CpuSetArg = arg;
            CT->SetProp(EProperty::CPU_SET);
        }

        SetMems(cfg.mems());

        return OK;
    }
} static CpuSet;

class TCpuSetAffinity : public TProperty {
public:
    TCpuSetAffinity() : TProperty(P_CPU_SET_AFFINITY, EProperty::NONE,
            "Resulting CPU affinity: [N,N-M,]...") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        auto lock = LockCpuAffinity();
        value = CT->CpuAffinity.Format();
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        auto lock = LockCpuAffinity();
        auto cfg = spec.mutable_cpu_set_affinity();
        for (auto cpu = 0u; cpu < CT->CpuAffinity.Size(); cpu++)
            if (CT->CpuAffinity.Get(cpu))
                cfg->add_cpu(cpu);
        cfg->set_list(CT->CpuAffinity.Format());
        cfg->set_count(CT->CpuAffinity.Weight());
    }
} static CpuSetAffinity;

class TIoLimit : public TProperty {
public:
    TIoLimit(std::string name, EProperty prop, std::string desc) :
        TProperty(name, prop, desc) {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportIoLimit() ||
         BlkioSubsystem.HasThrottler;
    }
    TError GetMap(const TUintMap &limit, std::string &value) {
        if (limit.size() == 1 && limit.count("fs")) {
            value = std::to_string(limit.at("fs"));
            return OK;
        }
        return UintMapToString(limit, value);
    }
    TError GetMapIndexed(const TUintMap &limit, const std::string &index, std::string &value) {
        if (!limit.count(index))
            return TError(EError::InvalidValue, "invalid index " + index);
        value = std::to_string(limit.at(index));
        return OK;
    }
    TError SetMapMap(TUintMap &limit, const TUintMap &map) {
        TError error;
        if (map.count("fs")) {
            error = CT->EnableControllers(CGROUP_MEMORY);
            if (error)
                return error;
        }
        if (map.size() > map.count("fs")) {
            error = CT->EnableControllers(CGROUP_BLKIO);
            if (error)
                return error;
        }
        limit = map;
        CT->SetProp(Prop);
        return OK;
    }
    TError SetMap(TUintMap &limit, const std::string &value) {
        TUintMap map;
        TError error;
        if (value.size() && value.find(':') == std::string::npos)
            error = StringToSize(value, map["fs"]);
        else
            error = StringToUintMap(value, map);
        if (error)
            return error;
        return SetMapMap(limit, map);
    }
    TError SetMapIndexed(TUintMap &limit, const std::string &index, const std::string &value) {
        TUintMap map = limit;
        TError error = StringToSize(value, map[index]);
        if (error)
            return error;
        return SetMapMap(limit, map);
    }
};

class TIoBpsLimit : public TIoLimit {
public:
    TIoBpsLimit()  : TIoLimit(P_IO_LIMIT, EProperty::IO_LIMIT,
            "IO bandwidth limit: fs|<path>|<disk> [r|w]: <bytes/s>;...")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        return GetMap(CT->IoBpsLimit, value);
    }
    TError Set(const std::string &value) {
        return SetMap(CT->IoBpsLimit, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        return GetMapIndexed(CT->IoBpsLimit, index, value);
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        return SetMapIndexed(CT->IoBpsLimit, index, value);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        DumpMap(CT->IoBpsLimit, *spec.mutable_io_limit());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_io_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TUintMap map;
        LoadMap(spec.io_limit(), CT->IoBpsLimit, map);
        return SetMapMap(CT->IoBpsLimit, map);
    }
} static IoBpsLimit;

class TIoOpsLimit : public TIoLimit {
public:
    TIoOpsLimit()  : TIoLimit(P_IO_OPS_LIMIT, EProperty::IO_OPS_LIMIT,
            "IOPS limit: fs|<path>|<disk> [r|w]: <iops>;...")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        return GetMap(CT->IoOpsLimit, value);
    }
    TError Set(const std::string &value) {
        return SetMap(CT->IoOpsLimit, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        return GetMapIndexed(CT->IoOpsLimit, index, value);
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        return SetMapIndexed(CT->IoOpsLimit, index, value);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        DumpMap(CT->IoOpsLimit, *spec.mutable_io_ops_limit());
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_io_ops_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TUintMap map;
        LoadMap(spec.io_ops_limit(), CT->IoOpsLimit, map);
        return SetMapMap(CT->IoOpsLimit, map);
    }
} static IoOpsLimit;

class TRespawn : public TProperty {
public:
    TRespawn() : TProperty(P_RESPAWN, EProperty::RESPAWN,
            "Automatically respawn dead container")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->AutoRespawn);
        return OK;
    }

    TError Set(bool value) {
        CT->AutoRespawn = value;
        CT->SetProp(EProperty::RESPAWN);
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_respawn(CT->AutoRespawn);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_respawn();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.respawn());
    }
} static Respawn;

class TRespawnCount : public TProperty {
public:
    TRespawnCount() : TProperty(P_RESPAWN_COUNT, EProperty::RESPAWN_COUNT,
            "Container respawn count")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->RespawnCount);
        return OK;
    }

    TError Set(uint64_t value) {
        CT->RespawnCount = value;
        CT->SetProp(EProperty::RESPAWN_COUNT);
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToUint64(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_respawn_count(CT->RespawnCount);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_respawn_count();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.respawn_count());
    }
} static RespawnCount;

class TRespawnLimit : public TProperty {
public:
    TRespawnLimit() : TProperty(P_RESPAWN_LIMIT, EProperty::RESPAWN_LIMIT,
            "Limit respawn count for specific container")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        if (CT->HasProp(EProperty::RESPAWN_LIMIT))
            value = std::to_string(CT->RespawnLimit);
        return OK;
    }

    TError Set(int64_t value) {
        CT->RespawnLimit = value;
        if (value >= 0)
            CT->SetProp(EProperty::RESPAWN_LIMIT);
        else
            CT->ClearProp(EProperty::RESPAWN_LIMIT);
        return OK;
    }

    TError Set(const std::string &value) {
        TError error;
        int64_t val;
        if (value != "") {
            error = StringToInt64(value, val);
            if (error)
                return error;
        }
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_max_respawns(CT->RespawnLimit);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_max_respawns();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.max_respawns());
    }
} static RespawnLimit;

class TRespawnDelay : public TProperty {
public:
    TRespawnDelay() : TProperty(P_RESPAWN_DELAY, EProperty::RESPAWN_DELAY,
            "Delay before automatic respawn")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        value = fmt::format("{}ns", CT->RespawnDelay);
        return OK;
    }

    TError Set(uint64_t value) {
        CT->RespawnDelay = value;
        CT->ClearProp(EProperty::RESPAWN_DELAY);
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToNsec(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_respawn_delay(CT->RespawnDelay);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_respawn_delay();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.respawn_delay());
    }
} static RespawnDelay;

class TPrivate : public TProperty {
public:
    TPrivate() : TProperty(P_PRIVATE, EProperty::PRIVATE,
            "User-defined property")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        value = CT->Private;
        return OK;
    }
    TError Set(const std::string &value) {
        if (value.length() > PRIVATE_VALUE_MAX)
            return TError(EError::InvalidValue, "Private value is too long, max {} bytes", PRIVATE_VALUE_MAX);
        CT->Private = value;
        CT->SetProp(EProperty::PRIVATE);
        return OK;
    }
    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_private_(CT->Private);
    }
    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_private_();
    }
    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.private_());
    }
} static Private;

class TLabels : public TProperty {
public:
    TLabels() : TProperty(P_LABELS, EProperty::LABELS, "User-defined labels")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        auto lock = LockContainers();
        value = StringMapToString(CT->Labels);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        auto lock = LockContainers();
        return CT->GetLabel(index, value);
    }
    TError Set(TStringMap &map, bool merge) {
        TError error;
        for (auto &it: map) {
            error = TContainer::ValidLabel(it.first, it.second);
            if (error)
                return error;
        }
        auto lock = LockContainers();
        auto count = CT->Labels.size();
        for (auto &it: map) {
            if (CT->Labels.find(it.first) == CT->Labels.end()) {
                if (it.second.size())
                    count++;
            } else if (!it.second.size())
                count--;
        }
        if (count > PORTO_LABEL_COUNT_MAX)
            return TError(EError::ResourceNotAvailable, "Too many labels");
            if (!merge) {
                for (auto &it: CT->Labels) {
                    if (map.find(it.first) == map.end())
                        map[it.first] = "";
                }
            }
        for (auto &it: map)
            CT->SetLabel(it.first, it.second);
        lock.unlock();
        for (auto &it: map)
            TContainerWaiter::ReportAll(*CT, it.first, it.second);
        return OK;
    }
    TError Set(const std::string &value) {
        TStringMap map;
        TError error = StringToStringMap(value, map);
        if (error)
            return error;
        return Set(map, true);
    }
    TError SetIndexed(const std::string &index, const std::string &value) {
        TError error = TContainer::ValidLabel(index, value);
        if (error)
            return error;
        auto lock = LockContainers();
        if (!value.empty() && CT->Labels.size() >= PORTO_LABEL_COUNT_MAX)
            return TError(EError::ResourceNotAvailable, "Too many labels");
        CT->SetLabel(index, value);
        lock.unlock();
        TContainerWaiter::ReportAll(*CT, index, value);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto lock = LockContainers();
        auto map = spec.mutable_labels();
        for (auto &it: CT->Labels) {
            auto l = map->add_map();
            l->set_key(it.first);
            l->set_val(it.second);
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_labels();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TStringMap map;
        for (auto &label: spec.labels().map())
            map[label.key()] = label.val();
        return Set(map, spec.labels().merge());
    }
} static Labels;

class TAgingTime : public TProperty {
public:
    TAgingTime() : TProperty(P_AGING_TIME, EProperty::AGING_TIME,
            "Remove dead containrs after [seconds]")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->AgingTime / 1000);
        return OK;
    }

    TError Set(uint64_t new_time) {
        CT->AgingTime = new_time * 1000;
        CT->SetProp(EProperty::AGING_TIME);
        return OK;
    }

    TError Set(const std::string &time) {
        uint64_t new_time;
        TError error = StringToUint64(time, new_time);
        if (error)
            return error;
        return Set(new_time);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_aging_time(CT->AgingTime / 1000);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_aging_time();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.aging_time());
    }
} static AgingTime;

class TEnablePorto : public TProperty {
public:
    TEnablePorto() : TProperty(P_ENABLE_PORTO, EProperty::ENABLE_PORTO,
            "Proto access level: false (none) | read-isolate | read-only | isolate | child-only | true (full)")
    {
        IsDynamic = true;
    }

    static bool Compatible(EAccessLevel parent, EAccessLevel child) {
        switch (parent) {
            case EAccessLevel::None:
                return child == EAccessLevel::None;
            case EAccessLevel::ReadIsolate:
            case EAccessLevel::ReadOnly:
                return child <= EAccessLevel::ReadOnly;
            default:
                return true;
        }
    }

    TError Get(std::string &value) {
        switch (CT->AccessLevel) {
            case EAccessLevel::None:
                value = "false";
                break;
            case EAccessLevel::ReadIsolate:
                value = "read-isolate";
                break;
            case EAccessLevel::ReadOnly:
                value = "read-only";
                break;
            case EAccessLevel::Isolate:
                value = "isolate";
                break;
            case EAccessLevel::SelfIsolate:
                value = "self-isolate";
                break;
            case EAccessLevel::ChildOnly:
                value = "child-only";
                break;
            default:
                value = "true";
                break;
        }
        return OK;
    }

    TError Set(const std::string &value) {
        EAccessLevel level;

        if (value == "false" || value == "none")
            level = EAccessLevel::None;
        else if (value == "read-isolate")
            level = EAccessLevel::ReadIsolate;
        else if (value == "read-only")
            level = EAccessLevel::ReadOnly;
        else if (value == "isolate")
            level = EAccessLevel::Isolate;
        else if (value == "self-isolate")
            level = EAccessLevel::SelfIsolate;
        else if (value == "child-only")
            level = EAccessLevel::ChildOnly;
        else if (value == "true" || value == "full")
            level = EAccessLevel::Normal;
        else
            return TError(EError::InvalidValue, "Unknown access level: " + value);

        if (level > EAccessLevel::None && !CL->IsSuperUser()) {
            for (auto p = CT->Parent; p; p = p->Parent)
                if (!Compatible(p->AccessLevel, level))
                    return TError(EError::Permission, "Parent container has lower access level");
        }

        CT->AccessLevel = level;
        CT->SetProp(EProperty::ENABLE_PORTO);
        return OK;
    }
    TError Start(void) {
        auto parent = CT->Parent;
        if (!Compatible(parent->AccessLevel, CT->AccessLevel))
            CT->AccessLevel = parent->AccessLevel;
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        std::string val;
        Get(val);
        spec.set_enable_porto(val);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_enable_porto();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.enable_porto());
    }
} static EnablePorto;

class TWeak : public TProperty {
public:
    TWeak() : TProperty(P_WEAK, EProperty::WEAK,
            "Destroy container when client disconnects")
    {
        IsDynamic = true;
        IsAnyState = true;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->IsWeak);
        return OK;
    }

    TError Set(bool value) {
        CT->IsWeak = value;
        CT->SetProp(EProperty::WEAK);
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_weak(CT->IsWeak);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_weak();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.weak());
    }
} static Weak;

/* Read-only properties derived from data filelds follow below... */

class TIdProperty : public TProperty {
public:
    TIdProperty() : TProperty(P_ID, EProperty::NONE,
            "Container id")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = fmt::format("{}", CT->Id);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_id(CT->Id);
    }
} static IdProperty;

class TLevelProperty : public TProperty {
public:
    TLevelProperty() : TProperty(P_LEVEL, EProperty::NONE,
            "Container level")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = fmt::format("{}", CT->Level);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_level(CT->Level);
    }
} static LevelProperty;

class TAbsoluteName : public TProperty {
public:
    TAbsoluteName() : TProperty(P_ABSOLUTE_NAME, EProperty::NONE,
            "Container name including porto namespaces") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->IsRoot())
            value = ROOT_CONTAINER;
        else
            value = ROOT_PORTO_NAMESPACE + CT->Name;
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        std::string val;
        Get(val);
        spec.set_absolute_name(val);
    }
} static AbsoluteName;

class TAbsoluteNamespace : public TProperty {
public:
    TAbsoluteNamespace() : TProperty(P_ABSOLUTE_NAMESPACE, EProperty::NONE,
            "Container namespace including parent namespaces") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = ROOT_PORTO_NAMESPACE + CT->GetPortoNamespace();
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        std::string val;
        Get(val);
        spec.set_absolute_namespace(val);
    }
} static AbsoluteNamespace;

class TState : public TProperty {
public:
    TState() : TProperty(P_STATE, EProperty::STATE, "container state")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = TContainer::StateName(CT->State);
        return OK;
    }
    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_state(TContainer::StateName(CT->State));
    }
} static State;

class TOomKilled : public TProperty {
public:
    TOomKilled() : TProperty(P_OOM_KILLED, EProperty::OOM_KILLED,
            "Container has been killed by OOM") {
        IsReadOnly = true;
        IsDeadOnly = true;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->OomKilled);
        return OK;
    }
    TError Set(const std::string &value) {
        return StringToBool(value, CT->OomKilled);
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_oom_killed(CT->OomKilled);
    }
} static OomKilled;

class TOomKills : public TProperty {
public:
    TOomKills() : TProperty(P_OOM_KILLS, EProperty::OOM_KILLS,
            "Count of tasks killed in container since start")
    {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        auto cg = MemorySubsystem.RootCgroup();
        uint64_t count;
        IsSupported = !MemorySubsystem.GetOomKills(cg, count);
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->OomKills);
        return OK;
    }
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToUint64(value, val);
        if (!error) {
            CT->OomKills = val;
            CT->SetProp(EProperty::OOM_KILLS);
        }
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_oom_kills(CT->OomKills);
    }
} static OomKills;

class TOomKillsTotal : public TProperty {
    public:
    TOomKillsTotal() : TProperty(P_OOM_KILLS_TOTAL, EProperty::OOM_KILLS_TOTAL,
            "Count of tasks killed in hierarchy since creation")
    {
        IsReadOnly = true;
    }
    void Init(void) {
        auto cg = MemorySubsystem.RootCgroup();
        uint64_t count;
        IsSupported = !MemorySubsystem.GetOomKills(cg, count);
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->OomKillsTotal);
        return OK;
    }
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToUint64(value, val);
        if (!error) {
            CT->OomKillsTotal = val;
            CT->SetProp(EProperty::OOM_KILLS_TOTAL);
        }
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_oom_kills_total(CT->OomKillsTotal);
    }
} static OomKillsTotal;

class TCoreDumped : public TProperty {
public:
    TCoreDumped() : TProperty(P_CORE_DUMPED, EProperty::NONE,
            "Main task dumped core at exit")
    {
        IsReadOnly = true;
        IsDeadOnly = true;
    }
    TError Get(std::string &value) {
        value = BoolToString(WIFSIGNALED(CT->ExitStatus) &&
                             WCOREDUMP(CT->ExitStatus));
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_core_dumped(WIFSIGNALED(CT->ExitStatus) && WCOREDUMP(CT->ExitStatus));
    }
} static CoreDumped;

class TOomIsFatal : public TProperty {
public:
    TOomIsFatal() : TProperty(P_OOM_IS_FATAL, EProperty::OOM_IS_FATAL,
            "Kill all affected containers on OOM event")
    {
        IsDynamic = true;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->OomIsFatal);
        return OK;
    }

    TError Set(bool value) {
        CT->OomIsFatal = value;
        CT->SetProp(EProperty::OOM_IS_FATAL);
        return OK;
    }

    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_oom_is_fatal(CT->OomIsFatal);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_oom_is_fatal();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.oom_is_fatal());
    }
} static OomIsFatal;

class TOomScoreAdj : public TProperty {
public:
    TOomScoreAdj() : TProperty(P_OOM_SCORE_ADJ, EProperty::OOM_SCORE_ADJ,
            "OOM score adjustment: -1000..1000") { }
    TError Get(std::string &value) {
        value = StringFormat("%d", CT->OomScoreAdj);
        return OK;
    }

    TError Set(int val) {
        if (val < -1000 || val > 1000)
            return TError(EError::InvalidValue, "out of range");
        if (CT->OomScoreAdj != val) {
            CT->OomScoreAdj = val;
            CT->SetProp(EProperty::OOM_SCORE_ADJ);
        }
        return OK;
    }

    TError Set(const std::string &value) {
        int val;
        TError error = StringToInt(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        spec.set_oom_score_adj(CT->OomScoreAdj);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_oom_score_adj();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.oom_score_adj());
    }

} static OomScoreAdj;

class TParent : public TProperty {
public:
    TParent() : TProperty(P_PARENT, EProperty::NONE,
            "Parent container absolute name")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->Level == 0)
            value = "";
        else if (CT->Level == 1)
            value = ROOT_CONTAINER;
        else
            value = ROOT_PORTO_NAMESPACE + CT->Parent->Name;
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        if (CT->Level == 1)
            spec.set_parent(ROOT_CONTAINER);
        else if (CT->Level > 1)
            spec.set_parent(ROOT_PORTO_NAMESPACE + CT->Parent->Name);
    }
} static Parent;

class TRootPid : public TProperty {
public:
    TRootPid() : TProperty(P_ROOT_PID, EProperty::NONE,
            "Main task pid")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }

    TError Get(pid_t &pid) {
        if (!CT->HasPidFor(*CL->ClientContainer))
            return TError(EError::Permission, "pid is unreachable");
        return CT->GetPidFor(CL->Pid, pid);
    }

    TError Get(std::string &value) {
        pid_t pid;
        auto error = Get(pid);
        if (!error)
            value = std::to_string(pid);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        pid_t pid;
        TError error = Get(pid);
        if (!error)
            spec.set_root_pid(pid);
    }
} static RootPid;

class TExitStatusProperty : public TProperty {
public:
    TExitStatusProperty() : TProperty(P_EXIT_STATUS, EProperty::EXIT_STATUS,
            "Main task exit status")
    {
        IsReadOnly = true;
        IsDeadOnly = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->ExitStatus);
        return OK;
    }
    TError Set(const std::string &value) {
        return StringToInt(value, CT->ExitStatus);
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_exit_status(CT->ExitStatus);
    }
} static ExitStatusProperty;

class TExitCodeProperty : public TProperty {
public:
    TExitCodeProperty() : TProperty(P_EXIT_CODE, EProperty::NONE,
            "Main task exit code, negative: exit signal, OOM: -99")
    {
        IsReadOnly = true;
        IsDeadOnly = true;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->GetExitCode());
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_exit_code(CT->GetExitCode());
    }
} static ExitCodeProperty;

class TStartErrorProperty : public TProperty {
public:
    TStartErrorProperty() : TProperty(P_START_ERROR, EProperty::NONE, "Last start error")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->StartError)
            value = CT->StartError.ToString();
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        if (CT->StartError)
            CT->StartError.Dump(*spec.mutable_start_error());
    }
} static StartErrorProperty;

class TMemUsage : public TProperty {
public:
    TMemUsage() : TProperty(P_MEMORY_USAGE, EProperty::NONE,
            "Memory usage [bytes]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }

    TError Get(uint64_t &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        return MemorySubsystem.Usage(cg, value);
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            spec.set_memory_usage(val);
    }
} static MemUsage;

class TMemReclaimed : public TProperty {
public:
    TMemReclaimed() : TProperty(P_MEMORY_RECLAIMED, EProperty::NONE,
            "Memory reclaimed from container [bytes]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        return MemorySubsystem.GetReclaimed(cg, val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            spec.set_memory_reclaimed(val);
    }
} static MemReclaimed;

class TAnonUsage : public TProperty {
public:
    TAnonUsage() : TProperty(P_ANON_USAGE, EProperty::NONE,
            "Anonymous memory usage [bytes]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        return MemorySubsystem.GetAnonUsage(cg, val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            spec.set_anon_usage(val);
    }
} static AnonUsage;

class TAnonMaxUsage : public TProperty {
public:
    TAnonMaxUsage() : TProperty(P_ANON_MAX_USAGE, EProperty::NONE,
            "Peak anonymous memory usage [bytes]")
    {
        IsRuntimeOnly = true;
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportAnonLimit();
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        return MemorySubsystem.GetAnonMaxUsage(cg, val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (error)
            return error;
        value = std::to_string(val);
        return OK;
    }
    TError Set(const std::string &) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        return MemorySubsystem.ResetAnonMaxUsage(cg);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            spec.set_anon_max_usage(val);
    }
} static AnonMaxUsage;

class TCacheUsage : public TProperty {
public:
    TCacheUsage() : TProperty(P_CACHE_USAGE, EProperty::NONE,
            "File cache usage [bytes]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        return MemorySubsystem.GetCacheUsage(cg, val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            spec.set_cache_usage(val);
    }
} static CacheUsage;

class THugetlbUsage : public TProperty {
public:
    THugetlbUsage() : TProperty(P_HUGETLB_USAGE, EProperty::NONE,
            "HugeTLB memory usage [bytes]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_HUGETLB;
    }
    void Init(void) {
        IsSupported = HugetlbSubsystem.Supported;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(HugetlbSubsystem);
        return HugetlbSubsystem.GetHugeUsage(cg, val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        TError error = Get(val);
        if (!val)
            spec.set_hugetlb_usage(val);
    }
} static HugetlbUsage;

class TMinorFaults : public TProperty {
public:
    TMinorFaults() : TProperty(P_MINOR_FAULTS, EProperty::NONE,
            "Minor page faults")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        TUintMap stat;
        TError error = MemorySubsystem.Statistics(cg, stat);
        if (error)
            return error;
        val = stat["total_pgfault"] - stat["total_pgmajfault"];
        return OK;
    }

    TError Get(std::string &value) {
            uint64_t val;
            TError error = Get(val);
            if (error)
                return error;
            value = std::to_string(val);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t v;
        TError error = Get(v);
        if (!error)
            spec.set_minor_faults(v);
    }
} static MinorFaults;

class TMajorFaults : public TProperty {
public:
    TMajorFaults() : TProperty(P_MAJOR_FAULTS, EProperty::NONE,
            "Major page faults")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        TUintMap stat;
        TError error = MemorySubsystem.Statistics(cg, stat);
        if (error)
            return error;
        val = stat["total_pgmajfault"];
        return OK;
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (error)
            return error;
        value = std::to_string(val);
        return OK;
    }
    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            spec.set_major_faults(val);
    }
} static MajorFaults;

class TVirtualMemory : public TProperty {
public:
    TVirtualMemory() : TProperty(P_VIRTUAL_MEMORY, EProperty::NONE,
            "Virtual memory size: <type>: <bytes>;...")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }
    TError Get(std::string &value) {
        TError error;
        TVmStat st;

        error = CT->GetVmStat(st);
        if (error)
            return error;

        UintMapToString(st.Stat, value);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TError error;
        TVmStat st;

        error = CT->GetVmStat(st);
        if (error)
            return error;
        auto it = st.Stat.find(index);
        if (it == st.Stat.end())
            return TError(EError::InvalidProperty, "Unknown {}", index);
        value = std::to_string(it->second);
        return OK;
    }
    void Dump(rpc::TContainerStatus &spec) override {
        TVmStat st;
        if (CT->GetVmStat(st))
            return;
        st.Dump(*spec.mutable_virtual_memory());
    }
} static VirtualMemory;

class TMaxRss : public TProperty {
public:
    TMaxRss() : TProperty(P_MAX_RSS, EProperty::NONE,
            "Peak anonymous memory usage [bytes] (legacy, use anon_max_usage)")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        TCgroup rootCg = MemorySubsystem.RootCgroup();
        TUintMap stat;
        IsSupported = MemorySubsystem.SupportAnonLimit() ||
            (!MemorySubsystem.Statistics(rootCg, stat) && stat.count("total_max_rss"));
    }
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        TError error = MemorySubsystem.GetAnonMaxUsage(cg, val);
        if (error) {
            TUintMap stat;
            error = MemorySubsystem.Statistics(cg, stat);
            val = stat["total_max_rss"];
        }
        value = std::to_string(val);
        return error;
    }
} static MaxRss;

class TCpuUsage : public TProperty {
public:
    TCpuUsage() : TProperty(P_CPU_USAGE, EProperty::NONE,
            "Consumed CPU time [nanoseconds]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_CPUACCT;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(CpuacctSubsystem);
        return CpuacctSubsystem.Usage(cg, val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            spec.set_cpu_usage(val);
    }
} static CpuUsage;

class TCpuSystem : public TProperty {
public:
    TCpuSystem() : TProperty(P_CPU_SYSTEM, EProperty::NONE,
            "Consumed system CPU time [nanoseconds]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_CPUACCT;
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(CpuacctSubsystem);
        return CpuacctSubsystem.SystemUsage(cg, val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            spec.set_cpu_usage_system(val);
    }
} static CpuSystem;

class TCpuWait : public TProperty {
public:
    TCpuWait() : TProperty(P_CPU_WAIT, EProperty::NONE,
            "CPU time waited for execution [nanoseconds]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_CPUACCT;
    }
    void Init(void) {
        IsSupported = CpuacctSubsystem.RootCgroup().Has("cpuacct.wait");
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(CpuacctSubsystem);
        return cg.GetUint64("cpuacct.wait", val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        TError error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            spec.set_cpu_wait(val);
    }
} static CpuWait;

class TCpuThrottled : public TProperty {
public:
    TCpuThrottled() : TProperty(P_CPU_THROTTLED, EProperty::NONE,
            "CPU throttled time [nanoseconds]")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_CPU;
    }
    void Init(void) {
        TUintMap stat;
        IsSupported = !CpuSubsystem.RootCgroup().GetUintMap("cpu.stat", stat) && stat.count("throttled_time");
    }

    TError Get(uint64_t &val) {
        auto cg = CT->GetCgroup(CpuSubsystem);
        TUintMap stat;
        TError error = cg.GetUintMap("cpu.stat", stat);
        if (!error)
            val = stat["throttled_time"];
        return error;
    }

    TError Get(std::string &value) {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            value = std::to_string(val);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        auto error = Get(val);
        if (!error)
            spec.set_cpu_throttled(val);
    }
} static CpuThrottled;

class TNetClassId : public TProperty {
public:
    TNetClassId() : TProperty(P_NET_CLASS_ID, EProperty::NONE,
            "Network class: major:minor (hex)")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }
    TError Get(std::string &value) {
        if (!CT->Net)
            return TError(EError::InvalidState, "not available");
        if (TNetClass::IsDisabled()) {
            value = "1:0";
            return OK;
        }
        TStringMap map;
        uint32_t id = CT->NetClass.MetaHandle;
        for (int cs = 0; cs < NR_TC_CLASSES; cs++)
            map[fmt::format("CS{}", cs)] = fmt::format("{:x}:{:x}", TC_H_MAJ(id + cs) >> 16, TC_H_MIN(id + cs));
        id = CT->NetClass.LeafHandle;
        for (int cs = 0; cs < NR_TC_CLASSES; cs++)
            map[fmt::format("Leaf CS{}", cs)] = fmt::format("{:x}:{:x}", TC_H_MAJ(id + cs) >> 16, TC_H_MIN(id + cs));
        value = StringMapToString(map);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        if (!CT->Net)
            return TError(EError::InvalidState, "not available");
        if (TNetClass::IsDisabled()) {
            value = "1:0";
            return OK;
        }
        for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
            uint32_t id = CT->NetClass.MetaHandle;
            if (index == fmt::format("CS{}", cs)) {
                value = fmt::format("{:x}:{:x}", TC_H_MAJ(id + cs) >> 16, TC_H_MIN(id + cs));
                return OK;
            }
            id = CT->NetClass.LeafHandle;
            if (index == fmt::format("Leaf CS{}", cs)) {
                value = fmt::format("{:x}:{:x}", TC_H_MAJ(id + cs) >> 16, TC_H_MIN(id + cs));
                return OK;
            }
        }
        return TError(EError::InvalidProperty, "Unknown network class");
    }
} NetClassId;

class TNetTos : public TProperty {
public:
    TNetTos() : TProperty(P_NET_TOS, EProperty::NET_TOS,
                          "Default IP TOS, format: CS0|...|CS7, default CS0") {
        IsDynamic = true;
        RequireControllers = CGROUP_NETCLS;
    }
    TError Get(std::string &value) {
        value = TNetwork::FormatTos(CT->NetClass.DefaultTos);
        return OK;
    }
    TError Set(const std::string &value) {
        int tos;
        TError error = TNetwork::ParseTos(value, tos);
        if (!error) {
            CT->NetClass.DefaultTos = tos;
            CT->SetProp(EProperty::NET_TOS);
        }
        return error;
    }
} static NetTos;

class TNetProperty : public TProperty {
    TUintMap TNetClass:: *Member;
public:
    TNetProperty(std::string name, TUintMap TNetClass:: *member, EProperty prop, std::string desc) :
        TProperty(name, prop, desc), Member(member)
    {
        IsDynamic = true;
        RequireControllers = CGROUP_NETCLS;
    }

    TError Set(TUintMap &map) {
        auto lock = TNetwork::LockNetState();
        auto &cur = CT->NetClass.*Member;
        if (cur != map) {
            CT->SetProp(Prop);
            cur = map;
        }
        return OK;
    }

    TError Set(const std::string &value) {
        TUintMap map;
        TError error = StringToUintMap(value, map);
        if (error)
            return error;
        return Set(map);
    }

    TError Get(std::string &value) {
        auto lock = TNetwork::LockNetState();
        return UintMapToString(CT->NetClass.*Member, value);
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        uint64_t val;
        TError error = StringToSize(value, val);
        if (error)
            return TError(EError::InvalidValue, "Invalid value " + value);

        auto lock = TNetwork::LockNetState();
        auto &cur = CT->NetClass.*Member;
        if (cur[index] != val) {
            CT->SetProp(Prop);
            cur[index] = val;
        }

        return OK;
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        auto lock = TNetwork::LockNetState();
        auto &cur = CT->NetClass.*Member;
        auto it = cur.find(index);
        if (it == cur.end())
            return TError(EError::InvalidValue, "invalid index " + index);
        value = std::to_string(it->second);
        return OK;
    }

    TError Start(void) {
        if (Prop == EProperty::NET_RX_LIMIT &&
                !CT->NetIsolate && CT->NetClass.RxLimit.size())
            return TError(EError::InvalidValue, "Net rx limit requires isolated network");
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        rpc::TUintMap *map;

        if (Name == P_NET_GUARANTEE)
            map = spec.mutable_net_guarantee();
        else if (Name == P_NET_LIMIT)
            map = spec.mutable_net_limit();
        else if (Name == P_NET_RX_LIMIT)
            map = spec.mutable_net_rx_limit();
        else
            return;

        auto lock = TNetwork::LockNetState();
        for (auto &it : CT->NetClass.*Member) {
            auto kv = map->add_map();
            kv->set_key(it.first);
            kv->set_val(it.second);
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        if (Name == P_NET_GUARANTEE)
            return spec.has_net_guarantee();
        if (Name == P_NET_LIMIT)
            return spec.has_net_limit();
        if (Name == P_NET_RX_LIMIT)
            return spec.has_net_rx_limit();
        return false;
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        TUintMap map;
        if (Name == P_NET_GUARANTEE)
            for (auto &kv: spec.net_guarantee().map())
                map[kv.key()] = kv.val();
        else if (Name == P_NET_LIMIT)
            for (auto &kv: spec.net_limit().map())
                map[kv.key()] = kv.val();
        else if (Name == P_NET_RX_LIMIT)
            for (auto &kv: spec.net_rx_limit().map())
                map[kv.key()] = kv.val();
        else
            return OK;
        return Set(map);
    }
};

TNetProperty NetGuarantee(P_NET_GUARANTEE, &TNetClass::TxRate, EProperty::NET_GUARANTEE,
        "Guaranteed network bandwidth: <interface>|default: <Bps>;...");

TNetProperty NetLimit(P_NET_LIMIT, &TNetClass::TxLimit, EProperty::NET_LIMIT,
        "Maximum network bandwidth: <interface>|default: <Bps>;...");

TNetProperty NetRxLimit(P_NET_RX_LIMIT, &TNetClass::RxLimit, EProperty::NET_RX_LIMIT,
        "Maximum ingress bandwidth: <interface>|default: <Bps>;...");

class TNetStatProperty : public TProperty {
public:
    uint64_t TNetStat:: *Member;
    bool ClassStat;
    bool SockStat;

    TNetStatProperty(std::string name, uint64_t TNetStat:: *member,
                     std::string desc) : TProperty(name, EProperty::NONE, desc) {
        Member = member;
        IsReadOnly = true;
        IsRuntimeOnly = true;
        ClassStat = Name == P_NET_BYTES || Name == P_NET_PACKETS ||
                    Name == P_NET_DROPS || Name == P_NET_OVERLIMITS;
        SockStat = Name == P_NET_BYTES || Name == P_NET_PACKETS ||
                   Name == P_NET_TX_BYTES || Name == P_NET_RX_BYTES ||
                   Name == P_NET_TX_PACKETS || Name == P_NET_RX_PACKETS;
    }

    TError Has() {
        if (ClassStat && !TNetClass::IsDisabled()) {
            if (CT->State == EContainerState::Stopped)
                return TError(EError::InvalidState, "Not available in stopped state");
            if (!(CT->Controllers & CGROUP_NETCLS))
                return TError(EError::ResourceNotAvailable, "RequireControllers is disabled");
            return OK;
        } else if (!CT->NetInherit || CT->IsRoot() || (SockStat && TNLinkSockDiag::IsEnabled()))
            return OK;

        return TError(EError::ResourceNotAvailable, "Shared network");
    }

    TError Get(TUintMap &stat) {
        auto lock = TNetwork::LockNetState();
        if (ClassStat && !TNetClass::IsDisabled()) {
            for (auto &it : CT->NetClass.Fold->ClassStat)
                stat[it.first] = &it.second->*Member;
        } else if (CT->Net && (!CT->Net->IsHost() || CT->IsRoot())) {
            for (auto &it: CT->Net->DeviceStat)
                stat[it.first] = &it.second->*Member;
            if (CT->IsRoot() && SockStat && TNLinkSockDiag::IsEnabled())
                stat["SockDiag"] = CT->SockStat.*Member;
        } else if (SockStat && TNLinkSockDiag::IsEnabled()) {
            stat["Uplink"] = CT->SockStat.*Member;
            stat["Latency"] = GetCurrentTimeMs() - CT->SockStat.UpdateTs;
        }
        return OK;
    }

    TError Get(std::string &value) {
        TUintMap stat;
        auto error = Get(stat);

        return UintMapToString(stat, value);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        auto lock = TNetwork::LockNetState();
        if (ClassStat && !TNetClass::IsDisabled()) {
            auto it = CT->NetClass.Fold->ClassStat.find(index);
            if (it == CT->NetClass.Fold->ClassStat.end())
                return TError(EError::InvalidValue, "network device " + index + " not found");
            value = std::to_string(it->second.*Member);
        } else if (CT->Net && (!CT->Net->IsHost() || CT->IsRoot())) {
            if (CT->IsRoot() && index == "SockDiag" && SockStat && TNLinkSockDiag::IsEnabled()) {
                value = std::to_string(CT->SockStat.*Member);
                return OK;
            }
            auto it = CT->Net->DeviceStat.find(index);
            if (it == CT->Net->DeviceStat.end())
                return TError(EError::InvalidValue, "network device " + index + " not found");
            value = std::to_string(it->second.*Member);
        } else if (SockStat && TNLinkSockDiag::IsEnabled()) {
            if (index == "Latency") {
                value = std::to_string(GetCurrentTimeMs() - CT->SockStat.UpdateTs);
            } else if (index == "Uplink") {
                value = std::to_string(CT->SockStat.*Member);
            } else {
                return TError(EError::InvalidValue, "network device " + index + " not found");
            }
        }
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        rpc::TUintMap *map;
        TUintMap stat;

        auto error = Get(stat);
        if (error)
            return;

        // FIXME
        if (Name == P_NET_BYTES)
            map = spec.mutable_net_bytes();
        else if (Name == P_NET_PACKETS)
            map = spec.mutable_net_packets();
        else if (Name == P_NET_OVERLIMITS)
            map = spec.mutable_net_overlimits();
        else if (Name == P_NET_DROPS)
            map = spec.mutable_net_drops();
        else if (Name == P_NET_TX_BYTES)
            map = spec.mutable_net_tx_bytes();
        else if (Name == P_NET_TX_PACKETS)
            map = spec.mutable_net_tx_packets();
        else if (Name == P_NET_TX_DROPS)
            map = spec.mutable_net_tx_drops();
        else if (Name == P_NET_RX_BYTES)
            map = spec.mutable_net_rx_bytes();
        else if (Name == P_NET_RX_PACKETS)
            map = spec.mutable_net_rx_packets();
        else if (Name == P_NET_RX_DROPS)
            map = spec.mutable_net_rx_drops();
        else
            return;

        for (auto &it: stat) {
            auto kv = map->add_map();
            kv->set_key(it.first);
            kv->set_val(it.second);
        }
    }
};

TNetStatProperty NetBytes(P_NET_BYTES, &TNetStat::TxBytes,
        "Class TX bytes: <interface>: <bytes>;...");
TNetStatProperty NetPackets(P_NET_PACKETS, &TNetStat::TxPackets,
        "Class TX packets: <interface>: <packets>;...");
TNetStatProperty NetDrops(P_NET_DROPS, &TNetStat::TxDrops,
        "Class TX drops: <interface>: <packets>;...");
TNetStatProperty NetOverlimits(P_NET_OVERLIMITS, &TNetStat::TxOverruns,
        "Class TX overlimits: <interface>: <packets>;...");

TNetStatProperty NetRxBytes(P_NET_RX_BYTES, &TNetStat::RxBytes,
        "Device RX bytes: <interface>: <bytes>;...");
TNetStatProperty NetRxPackets(P_NET_RX_PACKETS, &TNetStat::RxPackets,
        "Device RX packets: <interface>: <packets>;...");
TNetStatProperty NetRxDrops(P_NET_RX_DROPS, &TNetStat::RxDrops,
        "Device RX drops: <interface>: <packets>;...");

TNetStatProperty NetTxBytes(P_NET_TX_BYTES, &TNetStat::TxBytes,
        "Device TX bytes: <interface>: <bytes>;...");
TNetStatProperty NetTxPackets(P_NET_TX_PACKETS, &TNetStat::TxPackets,
        "Device TX packets: <interface>: <packets>;...");
TNetStatProperty NetTxDrops(P_NET_TX_DROPS, &TNetStat::TxDrops,
        "Device TX drops: <interface>: <packets>;...");

class TIoStat : public TProperty {
public:
    TIoStat(std::string name, EProperty prop, std::string desc) : TProperty(name, prop, desc) {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_MEMORY | CGROUP_BLKIO;
    }
    virtual TError GetMap(TUintMap &map) = 0;
    TError Get(std::string &value) {
        TUintMap map;
        TError error = GetMap(map);
        if (error)
            return error;
        return UintMapToString(map, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TUintMap map;
        TError error = GetMap(map);
        if (error)
            return error;

        if (map.find(index) != map.end()) {
            value = std::to_string(map[index]);
        } else {
            std::string disk, name;

            error = BlkioSubsystem.ResolveDisk(CL->ClientContainer->RootPath,
                                               index, disk);
            if (error)
                return error;
            error = BlkioSubsystem.DiskName(disk, name);
            if (error)
                return error;
            value = std::to_string(map[name]);
        }

        return OK;
    }

    void DumpMap(rpc::TUintMap &dump) {
        TUintMap map;
        GetMap(map);
        for (auto &it: map) {
            auto kv = dump.add_map();
            kv->set_key(it.first);
            kv->set_val(it.second);
        }
    }
};

class TIoReadStat : public TIoStat {
public:
    TIoReadStat() : TIoStat(P_IO_READ, EProperty::NONE,
            "Bytes read from disk: fs|hw|<disk>|<path>: <bytes>;...") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CT->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, TBlkioSubsystem::IoStat::Read, map);

        if (MemorySubsystem.SupportIoLimit()) {
            auto memCg = CT->GetCgroup(MemorySubsystem);
            TUintMap memStat;
            if (!MemorySubsystem.Statistics(memCg, memStat))
                map["fs"] = memStat["fs_io_bytes"] - memStat["fs_io_write_bytes"];
        }

        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        DumpMap(*spec.mutable_io_read());
    }
} static IoReadStat;

class TIoWriteStat : public TIoStat {
public:
    TIoWriteStat() : TIoStat(P_IO_WRITE, EProperty::NONE,
            "Bytes written to disk: fs|hw|<disk>|<path>: <bytes>;...") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CT->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, TBlkioSubsystem::IoStat::Write, map);

        if (MemorySubsystem.SupportIoLimit()) {
            auto memCg = CT->GetCgroup(MemorySubsystem);
            TUintMap memStat;
            if (!MemorySubsystem.Statistics(memCg, memStat))
                map["fs"] = memStat["fs_io_write_bytes"];
        }

        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        DumpMap(*spec.mutable_io_write());
    }
} static IoWriteStat;

class TIoOpsStat : public TIoStat {
public:
    TIoOpsStat() : TIoStat(P_IO_OPS, EProperty::NONE,
            "IO operations: fs|hw|<disk>|<path>: <ops>;...") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CT->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, TBlkioSubsystem::IoStat::Iops, map);

        if (MemorySubsystem.SupportIoLimit()) {
            auto memCg = CT->GetCgroup(MemorySubsystem);
            TUintMap memStat;
            if (!MemorySubsystem.Statistics(memCg, memStat))
                map["fs"] = memStat["fs_io_operations"];
        }

        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        DumpMap(*spec.mutable_io_ops());
    }
} static IoOpsStat;

class TIoTimeStat : public TIoStat {
public:
    TIoTimeStat() : TIoStat(P_IO_TIME, EProperty::NONE,
            "IO time: hw|<disk>|<path>: <nanoseconds>;...") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CT->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, TBlkioSubsystem::IoStat::Time, map);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        DumpMap(*spec.mutable_io_time());
    }
} static IoTimeStat;

class TTime : public TProperty {
public:
    TTime() : TProperty(P_TIME, EProperty::NONE, "Running time [seconds]")
    {
        IsReadOnly = true;
    }

    TError Get(int64_t &val) {
        if (CT->IsRoot()) {
            struct sysinfo si;
            if (sysinfo(&si))
                return TError::System("sysinfo");
            val = si.uptime;
            return OK;
        }
        if (CT->State == EContainerState::Stopped)
            val = 0;
        else if (CT->State == EContainerState::Dead)
            val = (CT->DeathTime - CT->StartTime) / 1000;
        else
            val = (GetCurrentTimeMs() - CT->StartTime) / 1000;
        return OK;

    }

    TError Get(std::string &value) {
        int64_t val;
        auto error = Get(val);
        if (error)
            return error;
        value = std::to_string(val);
        return OK;
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        if (index == "dead") {
            if (CT->State == EContainerState::Dead)
                value = std::to_string((GetCurrentTimeMs() - CT->DeathTime) / 1000);
            else
                return TError(EError::InvalidState, "Not dead yet");
        } else
            return TError(EError::InvalidValue, "What {}?", index);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        int64_t val;
        auto error = Get(val);
        if (error)
            return;
        spec.set_time(val);
        if (CT->State == EContainerState::Dead)
            spec.set_dead_time((GetCurrentTimeMs() - CT->DeathTime) / 1000);
    }
} static Time;

class TCreationTime : public TProperty {
public:
    TCreationTime() : TProperty(P_CREATION_TIME, EProperty::NONE, "Creation time") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = FormatTime(CT->RealCreationTime);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        if (index == "raw")
            value = std::to_string(CT->RealCreationTime);
        else
            return TError(EError::InvalidValue, "What {}?", index);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_creation_time(CT->RealCreationTime);
    }
} static CreationTime;

class TStartTime : public TProperty {
public:
    TStartTime() : TProperty(P_START_TIME, EProperty::NONE, "Start time") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->RealStartTime)
            value = FormatTime(CT->RealStartTime);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        if (index == "raw")
            value = std::to_string(CT->RealStartTime);
        else
            return TError(EError::InvalidValue, "What {}?", index);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        if (CT->RealStartTime)
            spec.set_start_time(CT->RealStartTime);
    }
} static StartTime;

class TDeathTime : public TProperty {
public:
    TDeathTime() : TProperty(P_DEATH_TIME, EProperty::NONE, "Death time") {
        IsReadOnly = true;
        IsDeadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->RealDeathTime)
            value = FormatTime(CT->RealDeathTime);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        if (index == "raw")
            value = std::to_string(CT->RealDeathTime);
        else
            return TError(EError::InvalidValue, "What {}?", index);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_death_time(CT->RealDeathTime);
    }
} static DeathTime;

class TChangeTime : public TProperty {
public:
    TChangeTime() : TProperty(P_CHANGE_TIME, EProperty::NONE, "Change time") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = FormatTime(CT->ChangeTime);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        if (index == "raw")
            value = std::to_string(CT->ChangeTime);
        else
            return TError(EError::InvalidValue, "What {}?", index);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        spec.set_change_time(CT->ChangeTime);
    }
} static ChangeTime;

class TPortoStat : public TProperty {
public:
    void Populate(TUintMap &m);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TPortoStat() : TProperty(P_PORTO_STAT, EProperty::NONE, "Porto statistics") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static PortoStat;

void TPortoStat::Populate(TUintMap &m) {
    m["spawned"] = Statistics->PortoStarts;
    m["errors"] = Statistics->Errors;
    m["cgerrors"] = Statistics->CgErrors;
    m["warnings"] = Statistics->Warns;
    m["fatals"] = Statistics->Fatals;
    m["taints"] = Statistics->Taints;
    m["postfork_issues"] = Statistics->PostForkIssues;
    m["master_uptime"] = (GetCurrentTimeMs() - Statistics->MasterStarted) / 1000;
    m["porto_uptime"] = (GetCurrentTimeMs() - Statistics->PortoStarted) / 1000;
    m["queued_statuses"] = Statistics->QueuedStatuses;
    m["queued_events"] = Statistics->QueuedEvents;
    m["remove_dead"] = Statistics->RemoveDead;
    m["restore_failed"] = Statistics->ContainerLost;
    uint64_t usage = 0;
    auto cg = MemorySubsystem.Cgroup(PORTO_DAEMON_CGROUP);
    TError error = MemorySubsystem.Usage(cg, usage);
    if (error)
        L_ERR("Can't get memory usage of portod");
    m["memory_usage_mb"] = usage / 1024 / 1024;

    usage = 0;
    cg = CpuacctSubsystem.Cgroup(PORTO_DAEMON_CGROUP);
    error = CpuacctSubsystem.Usage(cg, usage);
    if (error)
        L_ERR("Can't get cpu usage of portod");
    m["cpu_usage"] = usage;

    usage = 0;
    error = CpuacctSubsystem.SystemUsage(cg, usage);
    if (error)
        L_ERR("Can't get cpu system usage of portod");
    m["cpu_system_usage"] = usage;

    m["epoll_sources"] = Statistics->EpollSources;

    m["log_lines"] = Statistics->LogLines;
    m["log_bytes"] = Statistics->LogBytes;
    m["log_lines_lost"] = Statistics->LogLinesLost;
    m["log_bytes_lost"] = Statistics->LogBytesLost;
    m["log_open"] = Statistics->LogOpen;

    m["log_rotate_bytes"] = Statistics->LogRotateBytes;
    m["log_rotate_errors"] = Statistics->LogRotateErrors;

    m["containers"] = Statistics->ContainersCount - NR_SERVICE_CONTAINERS;

    m["containers_created"] = Statistics->ContainersCreated;
    m["containers_started"] = Statistics->ContainersStarted;
    m["containers_failed_start"] = Statistics->ContainersFailedStart;
    m["containers_oom"] = Statistics->ContainersOOM;
    m["containers_tainted"] = Statistics->ContainersTainted;

    m["running"] = RootContainer->RunningChildren;
    m["running_children"] = CT->RunningChildren;
    m["starting_children"] = CT->StartingChildren;

    m["layer_import"] = Statistics->LayerImport;
    m["layer_export"] = Statistics->LayerExport;
    m["layer_remove"] = Statistics->LayerRemove;

    m["volumes"] = Statistics->VolumesCount;
    m["volumes_created"] = Statistics->VolumesCreated;
    m["volumes_failed"] = Statistics->VolumesFailed;
    m["volume_links"] = Statistics->VolumeLinks;
    m["volume_links_mounted"] = Statistics->VolumeLinksMounted;
    m["volume_lost"] = Statistics->VolumeLost;

    m["volume_mounts"] = CT->VolumeMounts;

    m["networks"] = Statistics->NetworksCount;
    m["networks_created"] = Statistics->NetworksCreated;
    m["network_problems"] = Statistics->NetworkProblems;
    m["network_repairs"] = Statistics->NetworkRepairs;

    m["clients"] = Statistics->ClientsCount;
    m["clients_connected"] = Statistics->ClientsConnected;

    m["container_clients"] = CT->ClientsCount;
    m["container_oom"] = CT->OomEvents;
    m["container_requests"] = CT->ContainerRequests;

    m["requests_queued"] = Statistics->RequestsQueued;
    m["requests_completed"] = Statistics->RequestsCompleted;
    m["requests_failed"] = Statistics->RequestsFailed;

    m["fail_system"] = Statistics->FailSystem;
    m["fail_invalid_value"] = Statistics->FailInvalidValue;
    m["fail_invalid_command"] = Statistics->FailInvalidCommand;
    m["fail_memory_guarantee"] = Statistics->FailMemoryGuarantee;
    m["fail_invalid_netaddr"] = Statistics->FailInvalidNetaddr;

    m["requests_longer_1s"] = Statistics->RequestsLonger1s;
    m["requests_longer_3s"] = Statistics->RequestsLonger3s;
    m["requests_longer_30s"] = Statistics->RequestsLonger30s;
    m["requests_longer_5m"] = Statistics->RequestsLonger5m;
    m["longest_read_request"] = Statistics->LongestRoRequest;
    m["requests_top_running_time"] = RpcRequestsTopRunningTime() / 1000;

    m["spec_requests_completed"] = Statistics->SpecRequestsCompleted;
    m["spec_requests_longer_1s"] = Statistics->SpecRequestsLonger1s;
    m["spec_requests_longer_3s"] = Statistics->SpecRequestsLonger3s;
    m["spec_requests_longer_30s"] = Statistics->SpecRequestsLonger30s;
    m["spec_requests_longer_5m"] = Statistics->SpecRequestsLonger5m;
    m["spec_requests_failed"] = Statistics->SpecRequestsFailed;
    m["spec_fail_invalid_value"] = Statistics->SpecRequestsFailedInvalidValue;
    m["spec_fail_unknown"] = Statistics->SpecRequestsFailedUnknown;
    m["spec_fail_no_container"] = Statistics->SpecRequestsFailedContainerDoesNotExist;
}

TError TPortoStat::Get(std::string &value) {
    TUintMap m;
    Populate(m);

    return UintMapToString(m, value);
}

TError TPortoStat::GetIndexed(const std::string &index,
                                       std::string &value) {
    TUintMap m;
    Populate(m);

    if (m.find(index) == m.end())
        return TError(EError::InvalidValue, "Invalid subscript for property");

    value = std::to_string(m[index]);

    return OK;
}

class TProcessCount : public TProperty {
public:
    TProcessCount() : TProperty(P_PROCESS_COUNT, EProperty::NONE,
            "Process count")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_FREEZER;
    }

    TError Get(uint64_t &val) {
        return CT->GetProcessCount(val);
    }

    TError Get(std::string &value) {
        uint64_t val;
        auto error = Get(val);
        if (error)
            return error;
        value = std::to_string(val);
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        auto error = Get(val);
        if (error)
            return;
        spec.set_process_count(val);
    }
} static ProcessCount;

class TThreadCount : public TProperty {
public:
    TThreadCount() : TProperty(P_THREAD_COUNT, EProperty::NONE,
            "Thread count")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
        RequireControllers = CGROUP_FREEZER | CGROUP_PIDS;
    }

    TError Get(uint64_t &val) {
        return  CT->GetThreadCount(val);
    }

    TError Get(std::string &value) {
        uint64_t count;
        TError error = Get(count);
        if (!error)
            value = std::to_string(count);
        return error;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        uint64_t val;
        auto error = Get(val);
        if (error)
            return;
        spec.set_thread_count(val);
    }
} static ThreadCount;

class TThreadLimit : public TProperty {
public:
    TThreadLimit() : TProperty(P_THREAD_LIMIT, EProperty::THREAD_LIMIT,
            "Thread limit") {}
    void Init() {
        IsDynamic = true;
        IsSupported = PidsSubsystem.Supported;
        RequireControllers = CGROUP_PIDS;
    }
    TError Get(std::string &value) {
        if (CT->HasProp(EProperty::THREAD_LIMIT))
            value = std::to_string(CT->ThreadLimit);
        return OK;
    }

    TError Set(uint64_t val) {
        CT->ThreadLimit = val;
        CT->SetProp(EProperty::THREAD_LIMIT);
        return OK;
    }

    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToSize(value, val);
        if (error)
            return error;
        return Set(val);
    }

    void Dump(rpc::TContainerSpec &spec) override {
        if (CT->HasProp(EProperty::THREAD_LIMIT))
            spec.set_thread_limit(CT->ThreadLimit);
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_thread_limit();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        return Set(spec.thread_limit());
    }
} static ThreadLimit;

class TSysctlProperty : public TProperty {
public:
    TSysctlProperty() : TProperty(P_SYSCTL, EProperty::SYSCTL,
            "Sysctl, format: name: value;...") {}

    TError Get(std::string &value) {
        value = StringMapToString(CT->Sysctl);
        return OK;
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        auto it = CT->Sysctl.find(index);
        if (it != CT->Sysctl.end())
            value = it->second;
        else
            value = "";
        return OK;
    }

    TError Set(const std::string &value) {
        TStringMap map;
        TError error = StringToStringMap(value, map);
        if (error)
            return error;
        CT->Sysctl = map;
        CT->SetProp(EProperty::SYSCTL);
        return OK;
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        if (value == "")
            CT->Sysctl.erase(index);
        else
            CT->Sysctl[index] = value;
        CT->SetProp(EProperty::SYSCTL);
        return OK;
    }

    void Dump(rpc::TContainerSpec &spec) override {
        auto out = spec.mutable_sysctl();
        for (auto &it: CT->Sysctl) {
            auto s = out->add_map();
            s->set_key(it.first);
            s->set_val(it.second);
        }
    }

    bool Has(const rpc::TContainerSpec &spec) override {
        return spec.has_sysctl();
    }

    TError Load(const rpc::TContainerSpec &spec) override {
        if (!spec.sysctl().merge())
            CT->Sysctl.clear();
        for (auto &it: spec.sysctl().map()) {
            if (it.has_val())
                CT->Sysctl[it.key()] = it.val();
            else
                CT->Sysctl.erase(it.key());
        }
        CT->SetProp(EProperty::SYSCTL);
        return OK;
    }
} static SysctlProperty;

class TTaint : public TProperty {
public:
    TTaint() : TProperty(P_TAINT, EProperty::NONE, "Container problems") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        for (auto &taint: CT->Taint())
            value += taint + "\n";
        return OK;
    }

    void Dump(rpc::TContainerStatus &spec) override {
        for (auto &taint: CT->Taint()) {
            auto t = spec.add_taint();
            t->set_error(EError::Taint);
            t->set_msg(taint);
        }
    }
} static Taint;

void InitContainerProperties(void) {
    for (auto prop: ContainerProperties)
        prop.second->Init();
}
