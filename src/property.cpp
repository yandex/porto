#include "property.hpp"
#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "client.hpp"
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
    TError Set(const std::string &value) {
        double val;
        std::string unit;
        TError error = StringToValue(value, val, unit);
        if (error)
            return error;

        if (val < 0.01 || val > 100 || unit.size())
            return TError(EError::InvalidValue, "out of range");

        if (CT->IoWeight != val) {
            CT->IoWeight = val;
            CT->SetProp(EProperty::IO_WEIGHT);
        }

        return OK;
    }
} static IoWeight;

class TUser : public TProperty {
public:
    TUser() : TProperty(P_USER, EProperty::USER,
            "Start command with given user") {}
    TError Get(std::string &value) {
        value = UserName(CT->TaskCred.Uid);
        return OK;
    }
    TError Set(const std::string &username) {
        TCred cred;
        TError error = cred.Init(username);
        if (error) {
            cred.Gid = CT->TaskCred.Gid;
            error = UserId(username, cred.Uid);
            if (error)
                return error;
        } else if (CT->HasProp(EProperty::GROUP))
            cred.Gid = CT->TaskCred.Gid;
        CT->TaskCred = cred;
        CT->SetProp(EProperty::USER);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode)
            CT->TaskCred.Uid = RootUser;
        return OK;
    }
} static User;

class TGroup : public TProperty {
public:
    TGroup() : TProperty(P_GROUP, EProperty::GROUP, "Start command with given group") {}
    TError Get(std::string &value) {
        value = GroupName(CT->TaskCred.Gid);
        return OK;
    }
    TError Set(const std::string &groupname) {
        gid_t newGid;
        TError error = GroupId(groupname, newGid);
        if (error)
            return error;
        CT->TaskCred.Gid = newGid;
        CT->SetProp(EProperty::GROUP);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode)
            CT->TaskCred.Gid = RootGroup;
        return OK;
    }
} static Group;

class TOwnerUser : public TProperty {
public:
    TOwnerUser() : TProperty(P_OWNER_USER, EProperty::OWNER_USER,
            "Container owner user") {}

    TError Get(std::string &value) {
        value = UserName(CT->OwnerCred.Uid);
        return OK;
    }

    TError Set(const std::string &username) {
        TCred newCred;
        gid_t oldGid = CT->OwnerCred.Gid;
        TError error = newCred.Init(username);
        if (error)
            return error;

        /* try to preserve current group if possible */
        if (newCred.IsMemberOf(oldGid) ||
                CL->Cred.IsMemberOf(oldGid) ||
                CL->IsSuperUser())
            newCred.Gid = oldGid;

        error = CL->CanControl(newCred);
        if (error)
            return error;

        CT->OwnerCred = newCred;
        CT->SetProp(EProperty::OWNER_USER);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }
} static OwnerUser;

class TOwnerGroup : public TProperty {
public:
    TOwnerGroup() : TProperty(P_OWNER_GROUP, EProperty::OWNER_GROUP,
            "Container owner group") {}

    TError Get(std::string &value) {
        value = GroupName(CT->OwnerCred.Gid);
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

        CT->OwnerCred.Gid = newGid;
        CT->SetProp(EProperty::OWNER_GROUP);
        return OK;
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
    TError Set(const std::string &mem_guarantee) {
        uint64_t new_val;
        TError error = StringToSize(mem_guarantee, new_val);
        if (error)
            return error;

        CT->NewMemGuarantee = new_val;

        if (CT->State != EContainerState::Stopped) {
            error = CT->CheckMemGuarantee();
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
        CT->Command = command;
        CT->SetProp(EProperty::COMMAND);
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::COMMAND))
            CT->Command = "/sbin/init";
        return OK;
    }
} static Command;

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
    TError Set(const std::string &value) {
        uint64_t limit;
        TError error = StringToSize(value, limit);
        if (error)
            return error;

        uint64_t limit_max = config().container().stdout_limit_max();
        if (limit > limit_max && !CL->IsSuperUser())
            return TError(EError::Permission,
                          "Maximum limit is: " + std::to_string(limit_max));

        CT->Stdout.Limit = limit;
        CT->Stderr.Limit = limit;
        CT->SetProp(EProperty::STDOUT_LIMIT);
        return OK;
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
    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        if (val && (CT->HostMode || CT->JobMode))
            return TError(EError::InvalidValue, "isolate=true incompatible with virt_mode");
        CT->Isolate = val;
        CT->SetProp(EProperty::ISOLATE);
        CT->SanitizeCapabilitiesAll();
        return OK;
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
    TError Set(const std::string &net_desc) {
        auto new_net_desc = SplitEscapedString(net_desc, ' ', ';');
        TNetEnv NetEnv;
        TError error = NetEnv.ParseNet(new_net_desc);
        if (error)
            return error;
        if (!NetEnv.NetInherit && !NetEnv.NetNone) {
            error = CT->EnableControllers(CGROUP_NETCLS);
            if (error)
                return error;
        }
        CT->NetProp = new_net_desc; /* FIXME: Copy vector contents? */
        CT->NetIsolate = NetEnv.NetIsolate;
        CT->NetInherit = NetEnv.NetInherit;
        CT->SetProp(EProperty::NET);
        CT->SanitizeCapabilitiesAll();
        return OK;
    }
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::NET)) {
            CT->NetProp = { { "none" } };
            CT->NetIsolate = true;
            CT->NetInherit = false;
        }
        return OK;
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
    TError Set(const std::string &value) {
        TError error = StringToBool(value, CT->RootRo);
        if (!error)
            CT->SetProp(EProperty::ROOT_RDONLY);
        return error;
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
    TError Set(const std::string &value) {
        TError error = StringToOct(value, CT->Umask);
        if (error)
            return error;
        CT->SetProp(EProperty::UMASK);
        return OK;
    }
} static Umask;

class TControllers : public TProperty {
public:
    TControllers() : TProperty(P_CONTROLLERS, EProperty::CONTROLLERS,
            "Cgroup controllers") { }
    TError Get(std::string &value) {
        // TODO: backward compatibility, remove when porto with cgroup2 support will be on the whole cluster
        uint64_t controllers = CT->Controllers & ~CGROUP2;
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
        // TODO: backward compatibility, remove when porto with cgroup2 support will be on the whole cluster
        uint64_t controllers = CT->Controllers & ~CGROUP2;
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
        if (!env.GetEnv(index, value))
            return TError(EError::InvalidValue, "Variable " + index + " not defined");
        return OK;
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
} static EnvProperty;

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
} static Symlink;

class TIp : public TProperty {
public:
    TIp() : TProperty(P_IP, EProperty::IP,
            "IP configuration: <interface> <ip>/<prefix>; ...") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->IpList, ' ', ';');
        return OK;
    }
    TError Set(const std::string &ipaddr) {
        auto ipaddrs = SplitEscapedString(ipaddr, ' ', ';');
        TNetEnv NetEnv;
        TError error = NetEnv.ParseIp(ipaddrs);
        if (error)
            return error;
        CT->IpList = ipaddrs;
        CT->SetProp(EProperty::IP);
        return OK;
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
    TError Set(const std::string &value) {
        auto cfg = SplitEscapedString(value, ';', ' ');
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
} static IpLimit;

class TDefaultGw : public TProperty {
public:
    TDefaultGw() : TProperty(P_DEFAULT_GW, EProperty::DEFAULT_GW,
            "Default gateway: <interface> <ip>; ...") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->DefaultGw, ' ', ';');
        return OK;
    }
    TError Set(const std::string &gw) {
        TNetEnv NetEnv;
        auto gws = SplitEscapedString(gw, ' ', ';');
        TError error = NetEnv.ParseGw(gws);
        if (error)
            return error;
        CT->DefaultGw = gws;
        CT->SetProp(EProperty::DEFAULT_GW);
        return OK;
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
    TError Set(const std::string &value) {
        TUintMap val;
        TError error = StringToUintMap(value, val);
        if (!error) {
            auto lock = LockVolumes();
            CT->PlaceLimit = val;
            CT->SetProp(EProperty::PLACE_LIMIT);
        }
        return error;
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
} OwnedVolumes;

class TLinkedVolumes : public TProperty {
public:
    TLinkedVolumes() : TProperty(P_LINKED_VOLUMES, EProperty::NONE, "Linked volumes: volume [target] [ro] [!];...")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TMultiTuple links;

        for (auto &link: CT->VolumeLinks) {
            TPath path = link->Volume->ComposePath(*CL->ClientContainer);
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

        value = MergeEscapeStrings(links, ' ', ';');
        return OK;
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
} static RequiredVolumes;

class TMemoryLimit : public TProperty {
public:
    TMemoryLimit() : TProperty(P_MEM_LIMIT, EProperty::MEM_LIMIT,
            "Memory limit [bytes]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_MEMORY;
    }
    TError Get(std::string &value) {
        if (!CT->Level)
            value = std::to_string(GetTotalMemory() - GetHugetlbMemory());
        else
            value = std::to_string(CT->MemLimit);
        return OK;
    }
    TError Set(const std::string &limit) {
        uint64_t new_size = 0lu;
        TError error = StringToSize(limit, new_size);
        if (error)
            return error;
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
    TError Set(const std::string &limit) {
        uint64_t new_size;
        TError error = StringToSize(limit, new_size);
        if (error)
            return error;
        if (new_size && new_size < config().container().min_memory_limit())
            return TError(EError::InvalidValue, "Should be at least {}", config().container().min_memory_limit());
        if (CT->AnonMemLimit != new_size) {
            CT->AnonMemLimit = new_size;
            CT->SetProp(EProperty::ANON_LIMIT);
        }
        return OK;
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
    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (!error && val != CT->AnonOnly) {
            CT->AnonOnly = val;
            CT->SetProp(EProperty::ANON_ONLY);
        }
        return error;
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
    TError Set(const std::string &limit) {
        uint64_t new_size;
        TError error = StringToSize(limit, new_size);
        if (error)
            return error;
        if (new_size && new_size < config().container().min_memory_limit())
            return TError(EError::InvalidValue, "Should be at least {}", config().container().min_memory_limit());
        if (CT->DirtyMemLimit != new_size) {
            CT->DirtyMemLimit = new_size;
            CT->SetProp(EProperty::DIRTY_LIMIT);
        }
        return OK;
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
    TError Set(const std::string &value) {
        TError error;

        uint64_t limit;
        error = StringToSize(value, limit);
        if (error)
            return error;

        auto cg = CT->GetCgroup(HugetlbSubsystem);
        uint64_t usage;
        if (!HugetlbSubsystem.GetHugeUsage(cg, usage) && limit < usage)
            return TError(EError::InvalidValue, "current hugetlb usage is greater than limit");

        CT->HugetlbLimit = limit;
        CT->SetProp(EProperty::HUGETLB_LIMIT);
        return OK;
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
    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (!error && val != CT->RechargeOnPgfault) {
            CT->RechargeOnPgfault = val;
            CT->SetProp(EProperty::RECHARGE_ON_PGFAULT);
        }
        return error;
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
    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (!error && val != CT->PressurizeOnDeath) {
            CT->PressurizeOnDeath = val;
            CT->SetProp(EProperty::PRESSURIZE_ON_DEATH);
        }
        return error;
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
    TError Set(const std::string &value) {
        uint64_t limit;
        TError error = StringToCpuPower(value, limit);
        if (!error && CT->CpuLimit != limit) {
            CT->CpuLimit = limit;
            CT->SetProp(EProperty::CPU_LIMIT);
        }
        return error;
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
} static CpuGuaranteeTotal;

class TCpuPeriod : public TProperty {
public:
    TCpuPeriod() : TProperty(P_CPU_PERIOD, EProperty::CPU_PERIOD,
            "CPU limit period: 1ms..1s, default: 100ms [nanoseconds]")
    {
        IsDynamic = true;
        RequireControllers = CGROUP_CPU;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->CpuPeriod);
        return OK;
    }
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToNsec(value, val);
        if (error)
            return error;
        if (val < 1000000 || val > 1000000000)
            return TError(EError::InvalidValue, "cpu period out of range");
        if (CT->CpuPeriod != val) {
            CT->CpuPeriod = val;
            CT->SetProp(EProperty::CPU_PERIOD);
        }
        return OK;
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
    TError Set(const std::string &value) {
        double val;
        std::string unit;
        TError error = StringToValue(value, val, unit);
        if (error)
            return error;

        if (val < 0.01 || val > 100 || unit.size())
            return TError(EError::InvalidValue, "out of range");

        if (CT->CpuWeight != val) {
            CT->CpuWeight = val;
            CT->SetProp(EProperty::CPU_WEIGHT);
            CT->ChooseSchedPolicy();
        }
        return OK;
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
        return OK;
    }
    TError Set(const std::string &value) {
        auto cfg = SplitEscapedString(value, ' ');
        auto lock = LockCpuAffinity();
        TError error;

        ECpuSetType type;
        int arg = !CT->CpuSetArg;

        if (cfg.size() == 0 || cfg[0] == "all") {
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

        if (CT->CpuSetType != type || CT->CpuSetArg != arg) {
            CT->CpuSetType = type;
            CT->CpuSetArg = arg;
            CT->SetProp(EProperty::CPU_SET);
        }

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
    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        CT->AutoRespawn = val;
        CT->SetProp(EProperty::RESPAWN);
        return OK;
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
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToUint64(value, val);
        if (error)
            return error;
        CT->RespawnCount = val;
        CT->SetProp(EProperty::RESPAWN_COUNT);
        return OK;
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
    TError Set(const std::string &value) {
        TError error;
        int64_t val;
        if (value != "") {
            error = StringToInt64(value, val);
            if (error)
                return error;
        }
        CT->RespawnLimit = val;
        if (val >= 0)
            CT->SetProp(EProperty::RESPAWN_LIMIT);
        else
            CT->ClearProp(EProperty::RESPAWN_LIMIT);
        return OK;
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
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToNsec(value, val);
        if (error)
            return error;
        CT->RespawnDelay = val;
        CT->ClearProp(EProperty::RESPAWN_DELAY);
        return OK;
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
    TError Merge(const TStringMap &map) {
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
        return Merge(map);
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
    TError Set(const std::string &time) {
        uint64_t new_time;
        TError error = StringToUint64(time, new_time);
        if (error)
            return error;
        CT->AgingTime = new_time * 1000;
        CT->SetProp(EProperty::AGING_TIME);
        return OK;
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
    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (error)
            return error;
        CT->IsWeak = val;
        CT->SetProp(EProperty::WEAK);
        return OK;
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
    TError Set(const std::string &value) {
        bool val;
        TError error = StringToBool(value, val);
        if (!error) {
            CT->OomIsFatal = val;
            CT->SetProp(EProperty::OOM_IS_FATAL);
        }
        return error;
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
    TError Set(const std::string &value) {
        int val;
        TError error = StringToInt(value, val);
        if (error)
            return error;
        if (val < -1000 || val > 1000)
            return TError(EError::InvalidValue, "out of range");
        if (CT->OomScoreAdj != val) {
            CT->OomScoreAdj = val;
            CT->SetProp(EProperty::OOM_SCORE_ADJ);
        }
        return OK;
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
} static Parent;

class TRootPid : public TProperty {
public:
    TRootPid() : TProperty(P_ROOT_PID, EProperty::NONE,
            "Main task pid")
    {
        IsReadOnly = true;
        IsRuntimeOnly = true;
    }
    TError Get(std::string &value) {
        if (!CT->HasPidFor(*CL->ClientContainer))
            return TError(EError::Permission, "pid is unreachable");

        pid_t pid;
        TError error = CT->GetPidFor(CL->Pid, pid);
        if (!error)
            value = std::to_string(pid);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        TError error = MemorySubsystem.Usage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        TError error = MemorySubsystem.GetReclaimed(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        TError error = MemorySubsystem.GetAnonUsage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        TError error = MemorySubsystem.GetAnonMaxUsage(cg, val);
        if (error)
            return error;
        value = std::to_string(val);
        return OK;
    }
    TError Set(const std::string &) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        return MemorySubsystem.ResetAnonMaxUsage(cg);
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        TError error = MemorySubsystem.GetCacheUsage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(HugetlbSubsystem);
        uint64_t val;
        TError error = HugetlbSubsystem.GetHugeUsage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        TUintMap stat;
        if (MemorySubsystem.Statistics(cg, stat))
            value = "-1";
        else
            value = std::to_string(stat["total_pgfault"] - stat["total_pgmajfault"]);
        return OK;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        TUintMap stat;
        if (MemorySubsystem.Statistics(cg, stat))
            value = "-1";
        else
            value = std::to_string(stat["total_pgmajfault"]);
        return OK;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(CpuacctSubsystem);
        uint64_t val;
        TError error = CpuacctSubsystem.Usage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(CpuacctSubsystem);
        uint64_t val;
        TError error = CpuacctSubsystem.SystemUsage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(CpuacctSubsystem);
        TError error = cg.Get("cpuacct.wait", value);
        if (!error)
            value = StringTrim(value);
        return error;
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
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(CpuSubsystem);
        TUintMap stat;
        TError error = cg.GetUintMap("cpu.stat", stat);
        if (!error)
            value = std::to_string(stat["throttled_time"]);
        return error;
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
    TError Set(const std::string &value) {
        TUintMap map;
        TError error = StringToUintMap(value, map);
        if (error)
            return error;

        auto lock = TNetwork::LockNetState();
        auto &cur = CT->NetClass.*Member;
        if (cur != map) {
            CT->SetProp(Prop);
            cur = map;
        }

        return OK;
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

    TNetStatProperty(std::string name, uint64_t TNetStat:: *member,
                     std::string desc) : TProperty(name, EProperty::NONE, desc) {
        Member = member;
        IsReadOnly = true;
        IsRuntimeOnly = true;
        ClassStat = Name == P_NET_BYTES || Name == P_NET_PACKETS ||
                    Name == P_NET_DROPS || Name == P_NET_OVERLIMITS;
    }

    TError Has() {
        if (ClassStat) {
            if (CT->State == EContainerState::Stopped)
                return TError(EError::InvalidState, "Not available in stopped state");
            if (!(CT->Controllers & CGROUP_NETCLS))
                return TError(EError::ResourceNotAvailable, "RequireControllers is disabled");
            return OK;
        }

        if (!CT->NetInherit || CT->IsRoot())
            return OK;
        return TError(EError::ResourceNotAvailable, "Shared network");
    }

    TError Get(std::string &value) {
        TUintMap stat;
        auto lock = TNetwork::LockNetState();
        if (ClassStat && !TNetClass::IsDisabled()) {
            for (auto &it : CT->NetClass.Fold->ClassStat)
                stat[it.first] = &it.second->*Member;
        } else if (CT->Net) {
            for (auto &it: CT->Net->DeviceStat)
                stat[it.first] = &it.second->*Member;
        }
        return UintMapToString(stat, value);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        auto lock = TNetwork::LockNetState();
        if (ClassStat && !TNetClass::IsDisabled()) {
            auto it = CT->NetClass.Fold->ClassStat.find(index);
            if (it == CT->NetClass.Fold->ClassStat.end())
                return TError(EError::InvalidValue, "network device " + index + " not found");
            value = std::to_string(it->second.*Member);
        } else if (CT->Net) {
            auto it = CT->Net->DeviceStat.find(index);
            if (it == CT->Net->DeviceStat.end())
                return TError(EError::InvalidValue, "network device " + index + " not found");
            value = std::to_string(it->second.*Member);
        }
        return OK;
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
} static IoTimeStat;

class TTime : public TProperty {
public:
    TTime() : TProperty(P_TIME, EProperty::NONE, "Running time [seconds]")
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->IsRoot()) {
            struct sysinfo si;
            if (sysinfo(&si))
                return TError::System("sysinfo");
            value = std::to_string(si.uptime);
            return OK;
        }
        if (CT->State == EContainerState::Stopped)
            value = "0";
        else if (CT->State == EContainerState::Dead)
            value = std::to_string((CT->DeathTime - CT->StartTime) / 1000);
        else
            value = std::to_string((GetCurrentTimeMs() - CT->StartTime) / 1000);
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
    TError Get(std::string &value) {
        uint64_t count;
        TError error = CT->GetProcessCount(count);
        if (!error)
            value = std::to_string(count);
        return error;
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
    TError Get(std::string &value) {
        uint64_t count;
        TError error = CT->GetThreadCount(count);
        if (!error)
            value = std::to_string(count);
        return error;
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
    TError Set(const std::string &value) {
        uint64_t val;
        TError error = StringToSize(value, val);
        if (error)
            return error;
        CT->ThreadLimit = val;
        CT->SetProp(EProperty::THREAD_LIMIT);
        return OK;
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
} static Taint;

void InitContainerProperties(void) {
    for (auto prop: ContainerProperties)
        prop.second->Init();
}
