#include "property.hpp"
#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "client.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "network.hpp"
#include "statistics.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
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
    if (IsReadOnly)
        return TError(EError::InvalidValue, "Read-only value: " + Name);
    return TError(EError::NotSupported, "Not implemented: " + Name);
}

TError TProperty::GetIndexed(const std::string &, std::string &) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

TError TProperty::SetIndexed(const std::string &, const std::string &) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

TError TProperty::GetToSave(std::string &value) {
    if (Prop != EProperty::NONE)
        return Get(value);
    return TError("Trying to save non-serializable value");
}

TError TProperty::SetFromRestore(const std::string &value) {
    if (Prop != EProperty::NONE)
        return Set(value);
    return TError("Trying to restore non-serializable value");
}

/*
 * Note for properties:
 * Dead state 2-line check is mandatory for all properties
 * Some properties require to check if the property is supported
 * Some properties may forbid changing it in runtime
 * Of course, some properties can be read-only
 */

TError TProperty::IsAliveAndStopped(void) {
    if (CT->State != EContainerState::Stopped)
        return TError(EError::InvalidState,
                      "Cannot change property " + Name +
                      " for not stopped container, current state: " +
                      TContainer::StateName(CT->State));

    return OK;
}

TError TProperty::IsAlive(void) {
    if (CT->State == EContainerState::Dead)
        return TError(EError::InvalidState, "Cannot change property " + Name +
                      " while in the dead state");
    return OK;
}

TError TProperty::IsDead(void) {
    if (CT->State != EContainerState::Dead)
        return TError(EError::InvalidState, "Available only in dead state: " + Name +
                      ", current state: " + TContainer::StateName(CT->State));
    return OK;
}

TError TProperty::IsRunning(void) {
    /*
     * This snippet is taken from TContainer::GetProperty.
     * The method name misguides a bit, but may be the semantic
     * of such properties is that we can look at the value in
     * the dead state too...
     */
    if (CT->State == EContainerState::Stopped)
        return TError(EError::InvalidState, "Not available in stopped state: " + Name);
    return OK;
}

TError TProperty::WantControllers(uint64_t controllers) const {
    if (CT->State == EContainerState::Stopped) {
        CT->Controllers |= controllers;
        CT->RequiredControllers |= controllers;
    } else if ((CT->Controllers & controllers) != controllers)
        return TError(EError::NotSupported, "Cannot enable controllers in runtime");
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
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        if (limit.Permitted & ~AllCapabilities.Permitted) {
            limit.Permitted &= ~AllCapabilities.Permitted;
            return TError(EError::InvalidValue,
                          "Unsupported capability: " + limit.Format());
        }

        CT->CapLimit = limit;
        CT->SetProp(EProperty::CAPABILITIES);
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
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        if (ambient.Permitted & ~AllCapabilities.Permitted) {
            ambient.Permitted &= ~AllCapabilities.Permitted;
            return TError(EError::InvalidValue,
                          "Unsupported capability: " + ambient.Format());
        }

        CT->CapAmbient = ambient;
        CT->SetProp(EProperty::CAPABILITIES_AMBIENT);
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

class TCwd : public TProperty {
public:
    TCwd() : TProperty(P_CWD, EProperty::CWD, "Container working directory") {}
    TError Get(std::string &value) {
        value = CT->GetCwd().ToString();
        return OK;
    }
    TError Set(const std::string &cwd) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
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
            "Process limits: as|core|data|locks|memlock|nofile|nproc|stack: [soft]|unlimited [hard];... (see man prlimit) (dynamic)") {}

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
        TError error = IsAlive();
        if (error)
            return error;
        TUlimit lim;
        error = lim.Parse(value);
        if (error)
            return error;
        CT->Ulimit = lim;
        CT->SetProp(EProperty::ULIMIT);
        return OK;
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        TUlimit lim;
        error = lim.Parse(index + ":" + value);
        if (error)
            return error;
        CT->Ulimit.Merge(lim);
        CT->SetProp(EProperty::ULIMIT);
        return OK;
    }

} static Ulimit;

class TCpuPolicy : public TProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TCpuPolicy() : TProperty(P_CPU_POLICY, EProperty::CPU_POLICY,
            "CPU policy: rt, high, normal, batch, idle (dynamic)") {}
} static CpuPolicy;

TError TCpuPolicy::Set(const std::string &policy) {
    TError error = IsAlive();
    if (error)
        return error;

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

TError TCpuPolicy::Get(std::string &value) {
    value = CT->CpuPolicy;

    return OK;
}

class TIoPolicy : public TProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TIoPolicy() : TProperty(P_IO_POLICY, EProperty::IO_POLICY,
                            "IO policy: none | rt | high | normal | batch | idle (dynamic)") {}
} static IoPolicy;

TError TIoPolicy::Set(const std::string &policy) {
    int ioprio;

    TError error = IsAlive();
    if (error)
        return error;

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

TError TIoPolicy::Get(std::string &value) {
    value = CT->IoPolicy;

    return OK;
}

class TIoWeight : public TProperty {
public:
    TIoWeight() : TProperty(P_IO_WEIGHT, EProperty::IO_WEIGHT,
                            "IO weight: 0.01..100, default is 1 (dynamic)") {
        RequireControllers = CGROUP_BLKIO;
    }
    void Init(void) {
        IsSupported = BlkioSubsystem.HasWeight;
    }
    TError Get(std::string &value) {
        value = StringFormat("%lg", CT->IoWeight);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;

        error = WantControllers(CGROUP_BLKIO);
        if (error)
            return error;

        double val;
        std::string unit;
        error = StringToValue(value, val, unit);
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
    TUser() : TProperty(P_USER, EProperty::USER, "Start command with given user") {}

    TError Get(std::string &value) {
        value = UserName(CT->TaskCred.Uid);
        return OK;
    }

    TError Set(const std::string &username) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        TCred cred;
        error = cred.Init(username);
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
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        gid_t newGid;
        error = GroupId(groupname, newGid);
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
        CT->SanitizeCapabilities();
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
    TError Set(const std::string &mem_guarantee);
    TError Get(std::string &value);
    TMemoryGuarantee() : TProperty(P_MEM_GUARANTEE, EProperty::MEM_GUARANTEE,
                                    "Guaranteed amount of memory "
                                    "[bytes] (dynamic)")
    {
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
} static MemoryGuarantee;

TError TMemoryGuarantee::Set(const std::string &mem_guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_val = 0lu;
    error = StringToSize(mem_guarantee, new_val);
    if (error)
        return error;

    CT->NewMemGuarantee = new_val;

    if (CT->State != EContainerState::Stopped) {
        error = CT->CheckMemGuarantee();
        if (error) {
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

TError TMemoryGuarantee::Get(std::string &value) {
    value = std::to_string(CT->MemGuarantee);

    return OK;
}

class TMemTotalGuarantee : public TProperty {
public:
    TMemTotalGuarantee() : TProperty(P_MEM_TOTAL_GUARANTEE, EProperty::NONE,
            "Total memory guarantee for container hierarchy (ro)") {
        IsReadOnly = true;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
    TError Get(std::string &value) {
        auto val = CT->GetTotalMemGuarantee();
        if (val)
            value = std::to_string(val);
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
        TError error = IsAliveAndStopped();
        if (error)
            return error;
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
                           "Command for receiving core dump (dynamic)") {}
    void Init(void) {
        IsSupported = config().core().enable();
    }
    TError Get(std::string &command) {
        command = CT->CoreCommand;
        return OK;
    }
    TError Set(const std::string &command) {
        TError error = IsAlive();
        if (error)
            return error;
        CT->CoreCommand = command;
        CT->SetProp(EProperty::CORE_COMMAND);
        return OK;
    }
} static CoreCommand;

class TVirtMode : public TProperty {
public:
    TVirtMode() : TProperty(P_VIRT_MODE, EProperty::VIRT_MODE,
                            "Virtualization mode: os|app") {}
    TError Get(std::string &value) {
        value = CT->OsMode ? "os" : "app";
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        if (value == "app")
            CT->OsMode = false;
        else if (value == "os")
            CT->OsMode = true;
        else
            return TError(EError::InvalidValue, "Unknown: {}", value);
        CT->SetProp(EProperty::VIRT_MODE);
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
        TError error = IsAliveAndStopped();
        if (!error) {
            CT->Stdin.SetInside(path);
            CT->SetProp(EProperty::STDIN);
        }
        return error;
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
        TError error = IsAliveAndStopped();
        if (!error) {
            CT->Stdout.SetInside(path);
            CT->SetProp(EProperty::STDOUT);
        }
        return error;
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
        TError error = IsAliveAndStopped();
        if (!error) {
            CT->Stderr.SetInside(path);
            CT->SetProp(EProperty::STDERR);
        }
        return error;
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
            "Limit for stored stdout and stderr size (dynamic)") {}
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
    TStdoutOffset() : TProperty(D_STDOUT_OFFSET, EProperty::NONE,
            "Offset of stored stdout (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        value = std::to_string(CT->Stdout.Offset);
        return OK;
    }
} static StdoutOffset;

class TStderrOffset : public TProperty {
public:
    TStderrOffset() : TProperty(D_STDERR_OFFSET, EProperty::NONE,
            "Offset of stored stderr (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        value = std::to_string(CT->Stderr.Offset);
        return OK;
    }
} static StderrOffset;

class TStdout : public TProperty {
public:
    TStdout() : TProperty(D_STDOUT, EProperty::NONE,
            "stdout [[offset][:length]] (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CT->Stdout.Read(*CT, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CT->Stdout.Read(*CT, value, index);
    }
} static Stdout;

class TStderr : public TProperty {
public:
    TStderr() : TProperty(D_STDERR, EProperty::NONE,
            "stderr [[offset][:length]] (ro))") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CT->Stderr.Read(*CT, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
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
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        error = StringToBool(value, CT->BindDns);
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
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        bool val;
        error = StringToBool(value, val);
        if (error)
            return error;

        CT->Isolate = val;
        CT->SetProp(EProperty::ISOLATE);
        return OK;
    }
} static Isolate;

class TRoot : public TProperty {
public:
    TError Set(const std::string &root);
    TError Get(std::string &value);
    TRoot() : TProperty(P_ROOT, EProperty::ROOT, "Container chroot") {}
} static Root;

TError TRoot::Get(std::string &value) {
    value = CT->Root;

    return OK;
}

TError TRoot::Set(const std::string &root) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CT->Root = root;
    CT->SetProp(EProperty::ROOT);

    return OK;
}

class TNet : public TProperty {
public:
    TError Set(const std::string &net_desc);
    TError Get(std::string &value);
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
    TError Start(void) {
        if (CT->OsMode && !CT->HasProp(EProperty::NET)) {
            CT->NetProp = { { "none" } };
            CT->NetIsolate = true;
            CT->NetInherit = false;
        }
        return OK;
    }
} static Net;

TError TNet::Set(const std::string &net_desc) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    auto new_net_desc = SplitEscapedString(net_desc, ' ', ';');

    TNetEnv NetEnv;
    error = NetEnv.ParseNet(new_net_desc);
    if (error)
        return error;

    if (!NetEnv.NetInherit) {
        error = WantControllers(CGROUP_NETCLS);
        if (error)
            return error;
    }

    CT->NetProp = new_net_desc; /* FIXME: Copy vector contents? */
    CT->NetIsolate = NetEnv.NetIsolate;
    CT->NetInherit = NetEnv.NetInherit;
    CT->SetProp(EProperty::NET);
    return OK;
}

TError TNet::Get(std::string &value) {
    value = MergeEscapeStrings(CT->NetProp, ' ', ';');
    return OK;
}

class TRootRo : public TProperty {
public:
    TRootRo() : TProperty(P_ROOT_RDONLY, EProperty::ROOT_RDONLY,
                          "Mount root directory in read-only mode") {}
    TError Get(std::string &value) {
        value = BoolToString(CT->RootRo);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        error = StringToBool(value, CT->RootRo);
        if (!error)
            CT->SetProp(EProperty::ROOT_RDONLY);
        return error;
    }
} static RootRo;

class TUmask : public TProperty {
public:
    TUmask() : TProperty(P_UMASK, EProperty::UMASK, "Set file mode creation mask") { }
    TError Get(std::string &value) {
        value = StringFormat("%#o", CT->Umask);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        error = StringToOct(value, CT->Umask);
        if (error)
            return error;
        CT->SetProp(EProperty::UMASK);
        return OK;
    }
} static Umask;

class TControllers : public TProperty {
public:
    TControllers() : TProperty(P_CONTROLLERS, EProperty::CONTROLLERS, "Cgroup controllers") { }
    TError Get(std::string &value) {
        value = StringFormatFlags(CT->Controllers, ControllersName, ";");
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        uint64_t val;
        error = StringParseFlags(value, ControllersName, val, ';');
        if (error)
            return error;
        if ((val & CT->RequiredControllers) != CT->RequiredControllers)
            return TError(EError::InvalidValue, "Cannot disable required controllers");
        CT->Controllers = val;
        CT->SetProp(EProperty::CONTROLLERS);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        uint64_t val;
        TError error = StringParseFlags(index, ControllersName, val, ';');
        if (error)
            return error;
        value = BoolToString((CT->Controllers & val) == val);
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
    TCgroups() : TProperty(D_CGROUPS, EProperty::NONE, "cgroups (ro)") {
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
    TError Set(const std::string &hostname);
    TError Get(std::string &value);
    THostname() : TProperty(P_HOSTNAME, EProperty::HOSTNAME, "Container hostname") {}
} static Hostname;

TError THostname::Set(const std::string &hostname) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CT->Hostname = hostname;
    CT->SetProp(EProperty::HOSTNAME);

    return OK;
}

TError THostname::Get(std::string &value) {
    value = CT->Hostname;

    return OK;
}

class TEnvProperty : public TProperty {
public:
    TError Set(const std::string &env);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TError SetIndexed(const std::string &index, const std::string &env_val);
    TEnvProperty() : TProperty(P_ENV, EProperty::ENV,
                       "Container environment variables: <name>=<value>; ...") {}
} static EnvProperty;

TError TEnvProperty::Set(const std::string &env_val) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    auto envs = SplitEscapedString(env_val, ';');

    TEnv env;
    error =  env.Parse(envs, true);
    if (error)
        return error;

    env.Format(CT->EnvCfg);
    CT->SetProp(EProperty::ENV);

    return OK;
}

TError TEnvProperty::Get(std::string &value) {
    value = MergeEscapeStrings(CT->EnvCfg, ';');
    return OK;
}

TError TEnvProperty::SetIndexed(const std::string &index, const std::string &env_val) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TEnv env;
    error = env.Parse(CT->EnvCfg, true);
    if (error)
        return error;

    error = env.Parse({index + "=" + env_val}, true);
    if (error)
        return error;

    env.Format(CT->EnvCfg);
    CT->SetProp(EProperty::ENV);

    return OK;
}

TError TEnvProperty::GetIndexed(const std::string &index, std::string &value) {
    TEnv env;
    TError error = CT->GetEnvironment(env);
    if (error)
        return error;

    if (!env.GetEnv(index, value))
        return TError(EError::InvalidValue, "Variable " + index + " not defined");

    return OK;
}

class TBind : public TProperty {
public:
    TBind() : TProperty(P_BIND, EProperty::BIND, "Bind mounts: <source> <target> [ro|rw|rec]... ;...") {}
    TError Get(std::string &value) {
        value = TBindMount::Format(CT->BindMounts);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        std::vector<TBindMount> result;
        error = TBindMount::Parse(value, result);
        if (error)
            return error;
        CT->BindMounts = result;
        CT->SetProp(EProperty::BIND);
        return OK;
    }
} static Bind;

class TIp : public TProperty {
public:
    TError Set(const std::string &ipaddr);
    TError Get(std::string &value);
    TIp() : TProperty(P_IP, EProperty::IP,
                      "IP configuration: <interface> <ip>/<prefix>; ...") {}
} static Ip;

TError TIp::Set(const std::string &ipaddr) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    auto ipaddrs = SplitEscapedString(ipaddr, ' ', ';');

    TNetEnv NetEnv;
    error = NetEnv.ParseIp(ipaddrs);
    if (error)
        return error;

    CT->IpList = ipaddrs;
    CT->SetProp(EProperty::IP);

    return OK;
}

TError TIp::Get(std::string &value) {
    value = MergeEscapeStrings(CT->IpList, ' ', ';');
    return OK;
}

class TIpLimit : public TProperty {
public:
    TIpLimit() : TProperty(P_IP_LIMIT, EProperty::IP_LIMIT,
            "IP allowed for sub-containers: none|any|<ip>[/<mask>]; ...") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->IpLimit, ';');
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        auto list = SplitEscapedString(value, ';');

        for (auto &str: list) {
            if (str == "any" || str == "none")
                continue;
            TNlAddr addr;
            error = addr.Parse(AF_UNSPEC, str);
            if (error)
                return error;
            if (addr.Family() != AF_INET && addr.Family() != AF_INET6)
                return TError(EError::InvalidValue, "wrong address");
        }

        CT->IpLimit = list;
        CT->NetIpLimit = !list.empty() && list[0] != "any";
        CT->SetProp(EProperty::IP_LIMIT);

        return OK;
    }
} static IpLimit;

class TDefaultGw : public TProperty {
public:
    TError Set(const std::string &gw);
    TError Get(std::string &value);
    TDefaultGw() : TProperty(P_DEFAULT_GW, EProperty::DEFAULT_GW,
            "Default gateway: <interface> <ip>; ...") {}
} static DefaultGw;

TError TDefaultGw::Set(const std::string &gw) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TNetEnv NetEnv;
    auto gws = SplitEscapedString(gw, ' ', ';');

    error = NetEnv.ParseGw(gws);
    if (error)
        return error;

    CT->DefaultGw = gws;
    CT->SetProp(EProperty::DEFAULT_GW);

    return OK;
}

TError TDefaultGw::Get(std::string &value) {
    value = MergeEscapeStrings(CT->DefaultGw, ' ', ';');
    return OK;
}

class TResolvConf : public TProperty {
public:
    TResolvConf() : TProperty(P_RESOLV_CONF, EProperty::RESOLV_CONF,
            "DNS resolver configuration: default|keep|<resolv.conf option>;... (dynamic)") {}
    TError Get(std::string &value) {
        if (CT->ResolvConf.size() || CT->IsRoot())
            value = MergeEscapeStrings(CT->ResolvConf, ';');
        else if (CT->HasProp(EProperty::RESOLV_CONF))
            value = "keep";
        else if (CT->Root == "/")
            value = "inherit";
        else
            value = "default";
        return OK;
    }
    TError GetToSave(std::string &value) {
        value = MergeEscapeStrings(CT->ResolvConf, ';');
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
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
            CT->ResolvConf = SplitEscapedString(value, ';');
            CT->SetProp(EProperty::RESOLV_CONF);
        }
        return OK;
    }
} static ResolvConf;

class TDevicesProperty : public TProperty {
public:
    TDevicesProperty() : TProperty(P_DEVICES, EProperty::DEVICE_CONF,
            "Devices that container can access: <device> [r][w][m][-] [path] [mode] [user] [group]; ... (dynamic)")
    {
        RequireControllers = CGROUP_DEVICES;
    }
    TError Get(std::string &value) {
        value = CT->Devices.Format();
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        error = WantControllers(CGROUP_DEVICES);
        if (error)
            return error;
        TDevices devices;
        error = devices.Parse(value);
        if (error)
            return error;
        CT->Devices = devices;
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
    TError SetFromRestore(const std::string &value) {
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
    TError SetFromRestore(const std::string &value) {
        return StringToInt(value, CT->SeizeTask.Pid);
    }
} static SeizePid;

class TRawLoopDev : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRawLoopDev() : TProperty(P_RAW_LOOP_DEV, EProperty::LOOP_DEV, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static RawLoopDev;

TError TRawLoopDev::SetFromRestore(const std::string &value) {
    return StringToInt(value, CT->LoopDev);
}

TError TRawLoopDev::Get(std::string &value) {
    value = std::to_string(CT->LoopDev);

    return OK;
}

class TRawStartTime : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRawStartTime() : TProperty(P_RAW_START_TIME, EProperty::START_TIME, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static RawStartTime;

TError TRawStartTime::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CT->StartTime);
}

TError TRawStartTime::Get(std::string &value) {
    value = std::to_string(CT->StartTime);

    return OK;
}

class TRawDeathTime : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRawDeathTime() : TProperty(P_RAW_DEATH_TIME, EProperty::DEATH_TIME, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static RawDeathTime;

TError TRawDeathTime::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CT->DeathTime);
}

TError TRawDeathTime::Get(std::string &value) {
    value = std::to_string(CT->DeathTime);

    return OK;
}

class TPortoNamespace : public TProperty {
public:
    TPortoNamespace() : TProperty(P_PORTO_NAMESPACE, EProperty::PORTO_NAMESPACE,
            "Porto containers namespace (container name prefix) (deprecated, use enable_porto=isolate instead)") {}
    TError Get(std::string &value) {
        value = CT->NsName;
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        CT->NsName = value;
        CT->SetProp(EProperty::PORTO_NAMESPACE);
        return OK;
    }
} static PortoNamespace;

class TPlaceProperty : public TProperty {
public:
    TPlaceProperty() : TProperty(P_PLACE, EProperty::PLACE,
        "Places for volumes and layers: [default][;allowed...]") {}
    TError Get(std::string &value) {
        value = MergeEscapeStrings(CT->Place, ';');
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        CT->Place = SplitEscapedString(value, ';');
        CT->SetProp(EProperty::PLACE);
        return OK;
    }
} static PlaceProperty;

class TPlaceLimit : public TProperty {
public:
    TPlaceLimit() : TProperty(P_PLACE_LIMIT, EProperty::PLACE_LIMIT,
            "Limits sum of volume space_limit: total|default|/place|tmpfs|lvm group|rbd: bytes;... (dynamic)") {}
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
        TError error = IsAlive();
        if (error)
            return error;
        TUintMap val;
        error = StringToUintMap(value, val);
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
    TPlaceUsage() : TProperty(D_PLACE_USAGE, EProperty::NONE,
            "Current sum of volume space_limit: total|/place|tmpfs|lvm group|rbd: bytes;... (ro)") {
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

class TVolumesList : public TProperty {
    std::list<std::shared_ptr<TVolume>> TContainer:: *Member;
public:
    TVolumesList(std::string name,
            std::list<std::shared_ptr<TVolume>> TContainer:: *member,
            std::string desc) :
        TProperty(name, EProperty::NONE, desc), Member(member)
    {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TTuple paths;

        auto lock = LockVolumes();
        for (auto &vol: CT->*Member) {
            TPath path = CL->ComposePath(vol->Path);
            if (!path)
                path = "@" + vol->Path.ToString();
            paths.push_back(path.ToString());
        }

        value = MergeEscapeStrings(paths, ';');
        return OK;
    }
};

static TVolumesList OwnerVolumes(D_OWNED_VOLUMES, &TContainer::OwnedVolumes,
                                 "Owned volumes: volume;... (ro)");
static TVolumesList LinedVolumes(D_LINKED_VOLUMES, &TContainer::LinkedVolumes,
                                 "Linked volumes: volume;... (ro)");

class TRequiredVolumes : public TProperty {
public:
    TRequiredVolumes() :
        TProperty(P_REQUIRED_VOLUMES, EProperty::REQUIRED_VOLUMES,
                 "Required volumes: volume;... (dynamic)") {}
    TError Get(std::string &value) {
        TTuple paths;

        auto lock = LockVolumes();
        for (auto &path: CT->RequiredVolumes) {
            TPath p = CL->ComposePath(path);
            if (!p)
                p = "%" + path;
            paths.push_back(p.ToString());
        }

        value = MergeEscapeStrings(paths, ';');
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;

        TTuple paths = SplitEscapedString(value, ';');
        for (auto &path: paths) {
            if (path[0] == '%')
                path = path.substr(1);
            else
                path = CL->ResolvePath(path).ToString();
        }

        auto lock = LockVolumes();
        if (CT->State != EContainerState::Stopped) {
            error = TVolume::CheckRequired(paths);
            if (error)
                return error;
        }
        CT->RequiredVolumes = paths;
        CT->SetProp(EProperty::REQUIRED_VOLUMES);
        return OK;
    }
} static RequiredVolumes;

class TMemoryLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TMemoryLimit() : TProperty(P_MEM_LIMIT, EProperty::MEM_LIMIT,
                               "Memory hard limit [bytes] (dynamic)")
    {
        RequireControllers = CGROUP_MEMORY;
    }
} static MemoryLimit;

TError TMemoryLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (new_size && new_size < config().container().min_memory_limit())
        return TError(EError::InvalidValue, "Should be at least " +
                std::to_string(config().container().min_memory_limit()));

    if (CT->MemLimit != new_size) {
        CT->MemLimit = new_size;
        CT->SetProp(EProperty::MEM_LIMIT);
    }

    return OK;
}

TError TMemoryLimit::Get(std::string &value) {
    if (CT->IsRoot())
        value = std::to_string(GetTotalMemory());
    else
        value = std::to_string(CT->MemLimit);
    return OK;
}

class TAnonLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TAnonLimit() : TProperty(P_ANON_LIMIT, EProperty::ANON_LIMIT,
                             "Anonymous memory limit [bytes] (dynamic)")
    {
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportAnonLimit();
    }

} static AnonLimit;

TError TAnonLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (new_size && new_size < config().container().min_memory_limit())
        return TError(EError::InvalidValue, "Should be at least " +
                std::to_string(config().container().min_memory_limit()));

    if (CT->AnonMemLimit != new_size) {
        CT->AnonMemLimit = new_size;
        CT->SetProp(EProperty::ANON_LIMIT);
    }

    return OK;
}

TError TAnonLimit::Get(std::string &value) {
    value = std::to_string(CT->AnonMemLimit);

    return OK;
}

class TDirtyLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TDirtyLimit() : TProperty(P_DIRTY_LIMIT, EProperty::DIRTY_LIMIT,
                              "Dirty file cache limit [bytes] "
                              "(dynamic)" )
    {
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsHidden = !MemorySubsystem.SupportDirtyLimit();
    }
} static DirtyLimit;

TError TDirtyLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (new_size && new_size < config().container().min_memory_limit())
        return TError(EError::InvalidValue, "Should be at least " +
                std::to_string(config().container().min_memory_limit()));

    if (CT->DirtyMemLimit != new_size) {
        CT->DirtyMemLimit = new_size;
        CT->SetProp(EProperty::DIRTY_LIMIT);
    }

    return OK;
}

TError TDirtyLimit::Get(std::string &value) {
    value = std::to_string(CT->DirtyMemLimit);

    return OK;
}

class THugetlbLimit : public TProperty {
public:
    THugetlbLimit() : TProperty(P_HUGETLB_LIMIT, EProperty::HUGETLB_LIMIT,
                                "Hugetlb memory limit [bytes] (dynamic)")
    {
        RequireControllers = CGROUP_HUGETLB;
    }
    void Init(void) {
        IsSupported = HugetlbSubsystem.Supported;
    }
    TError Get(std::string &value) {
        if (CT->HasProp(EProperty::HUGETLB_LIMIT))
            value = std::to_string(CT->HugetlbLimit);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        error = WantControllers(CGROUP_HUGETLB);
        if (error)
            return error;
        if (value.empty()) {
            CT->HugetlbLimit = -1;
            CT->ClearProp(EProperty::HUGETLB_LIMIT);
        } else {
            uint64_t limit = 0lu;
            error = StringToSize(value, limit);
            if (error)
                return error;

            auto cg = CT->GetCgroup(HugetlbSubsystem);
            uint64_t usage;
            if (!HugetlbSubsystem.GetHugeUsage(cg, usage) && limit < usage)
                return TError(EError::InvalidValue,
                              "current hugetlb usage is greater than limit");

            CT->HugetlbLimit = limit;
            CT->SetProp(EProperty::HUGETLB_LIMIT);
        }
        return OK;
    }
} static HugetlbLimit;

class TRechargeOnPgfault : public TProperty {
public:
    TError Set(const std::string &recharge);
    TError Get(std::string &value);
    TRechargeOnPgfault() : TProperty(P_RECHARGE_ON_PGFAULT,
                                     EProperty::RECHARGE_ON_PGFAULT,
                                     "Recharge memory on "
                                     "page fault (dynamic)")
    {
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportRechargeOnPgfault();
    }
} static RechargeOnPgfault;

TError TRechargeOnPgfault::Set(const std::string &recharge) {
    TError error = IsAlive();
    if (error)
        return error;

    error = WantControllers(CGROUP_MEMORY);
    if (error)
        return error;

    bool new_val;
    if (recharge == "true")
        new_val = true;
    else if (recharge == "false")
        new_val = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    if (CT->RechargeOnPgfault != new_val) {
        CT->RechargeOnPgfault = new_val;
        CT->SetProp(EProperty::RECHARGE_ON_PGFAULT);
    }

    return OK;
}

TError TRechargeOnPgfault::Get(std::string &value) {
    value = CT->RechargeOnPgfault ? "true" : "false";

    return OK;
}

class TPressurizeOnDeath : public TProperty {
public:
    TPressurizeOnDeath() : TProperty(P_PRESSURIZE_ON_DEATH, EProperty::PRESSURIZE_ON_DEATH,
                                     "After death set tiny soft memory limit (dynamic)") {
        RequireControllers = CGROUP_MEMORY;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->PressurizeOnDeath);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        bool val;
        error = StringToBool(value, val);
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
                            "CPU limit: 0-100.0 [%] | 0.0c-<CPUS>c "
                            " [cores] (dynamic)")
    {
        RequireControllers = CGROUP_CPU;
    }
    TError Get(std::string &value) {
        value = CpuPowerToString(CT->CpuLimit);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;

        error = WantControllers(CGROUP_CPU);
        if (error)
            return error;

        uint64_t limit;
        error = StringToCpuPower(value, limit);
        if (error)
            return error;

        if (CT->CpuLimit != limit) {
            CT->CpuLimit = limit;
            CT->SetProp(EProperty::CPU_LIMIT);
        }

        return OK;
    }
} static CpuLimit;

class TCpuLimitTotal : public TProperty {
public:
    TCpuLimitTotal() : TProperty(P_CPU_TOTAL_LIMIT, EProperty::NONE,
                                     "CPU total limit: <CPUS>c [cores] (ro)")
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
                                "CPU guarantee: 0-100.0 [%] | "
                                "0.0c-<CPUS>c [cores] (dynamic)")
    {
        RequireControllers = CGROUP_CPU;
    }
    TError Get(std::string &value) {
        value = CpuPowerToString(CT->CpuGuarantee);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        error = WantControllers(CGROUP_CPU);
        if (error)
            return error;
        uint64_t guarantee;
        error = StringToCpuPower(value, guarantee);
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
                                     "CPU total guarantee: <CPUS>c [cores] (ro)")
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
            "CPU limit period: 1ms..1s, default: 100ms [nanoseconds] (dynamic)")
    {
        RequireControllers = CGROUP_CPU;
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->CpuPeriod);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        error = WantControllers(CGROUP_CPU);
        if (error)
            return error;
        uint64_t val;
        error = StringToNsec(value, val);
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
                            "CPU weight 0.01..100, default is 1 (dynamic)") {}
    TError Get(std::string &value) {
        value = StringFormat("%lg", CT->CpuWeight);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;

        double val;
        std::string unit;
        error = StringToValue(value, val, unit);
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
            "CPU set: [N|N-M,]... | node N | reserve N | threads N | cores N (dynamic)")
    {
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
        TError error = IsAlive();
        if (error)
            return error;
        error = WantControllers(CGROUP_CPUSET);
        if (error)
            return error;

        auto cfg = SplitEscapedString(value, ' ');

        auto lock = LockCpuAffinity();

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
    TCpuSetAffinity() : TProperty(D_CPU_SET_AFFINITY, EProperty::NONE,
            "Resulting CPU affinity: [N,N-M,]... (ro)") {
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
        TError error = IsAlive();
        if (error)
            return error;
        if (map.count("fs")) {
            error = WantControllers(CGROUP_MEMORY);
            if (error)
                return error;
        }
        if (map.size() > map.count("fs")) {
            error = WantControllers(CGROUP_BLKIO);
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
            "IO bandwidth limit: fs|<path>|<disk> [r|w]: <bytes/s>;... (dynamic)") {}
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
            "IOPS limit: fs|<path>|<disk> [r|w]: <iops>;... (dynamic)") {}
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
                           "Automatically respawn dead container (dynamic)") {}
    TError Get(std::string &value) {
        value = BoolToString(CT->AutoRespawn);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;
        bool val;
        error = StringToBool(value, val);
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
                                "current respawn count (dynamic)") {
    }
    TError Get(std::string &value) {
        value = std::to_string(CT->RespawnCount);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error;
        uint64_t val;
        error = StringToUint64(value, val);
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
            "Limit respawn count for specific container (dynamic)") {}
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
                               "Delay before automatic respawn (dynamic)") {}
    TError Get(std::string &value) {
        value = fmt::format("{}ns", CT->RespawnDelay);
        return OK;
    }
    TError Set(const std::string &value) {
        TError error;
        uint64_t val;
        error = StringToNsec(value, val);
        if (error)
            return error;
        CT->RespawnDelay = val;
        CT->ClearProp(EProperty::RESPAWN_DELAY);
        return OK;
    }
} static RespawnDelay;

class TPrivate : public TProperty {
public:
    TError Set(const std::string &max);
    TError Get(std::string &value);
    TPrivate() : TProperty(P_PRIVATE, EProperty::PRIVATE,
                           "User-defined property (dynamic)") {}
} static Private;

TError TPrivate::Set(const std::string &value) {
    TError error = IsAlive();
    if (error)
        return error;

    uint32_t max = config().container().private_max();
    if (value.length() > max)
        return TError(EError::InvalidValue, "Value is too long");

    CT->Private = value;
    CT->SetProp(EProperty::PRIVATE);

    return OK;
}

TError TPrivate::Get(std::string &value) {
    value = CT->Private;

    return OK;
}

class TAgingTime : public TProperty {
public:
    TError Set(const std::string &time);
    TError Get(std::string &value);
    TAgingTime() : TProperty(P_AGING_TIME, EProperty::AGING_TIME,
      "Remove dead containrs after [seconds] (dynamic)") {}
} static AgingTime;

TError TAgingTime::Set(const std::string &time) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_time;
    error = StringToUint64(time, new_time);
    if (error)
        return error;

    CT->AgingTime = new_time * 1000;
    CT->SetProp(EProperty::AGING_TIME);

    return OK;
}

TError TAgingTime::Get(std::string &value) {
    value = std::to_string(CT->AgingTime / 1000);

    return OK;
}

class TEnablePorto : public TProperty {
public:
    TEnablePorto() : TProperty(P_ENABLE_PORTO, EProperty::ENABLE_PORTO,
            "Proto access level: false (none) | read-isolate | read-only | isolate | child-only | true (full) (dynamic)") {}

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
    TError Set(const std::string &weak);
    TError Get(std::string &value);
    TWeak() : TProperty(P_WEAK, EProperty::WEAK,
                        "Destroy container when client disconnects (dynamic)") {}
} static Weak;

TError TWeak::Set(const std::string &weak) {
    TError error = IsAlive();
    if (error)
        return error;

    if (weak == "true")
        CT->IsWeak = true;
    else if (weak == "false")
        CT->IsWeak = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CT->SetProp(EProperty::WEAK);

    return OK;
}

TError TWeak::Get(std::string &value) {
    value = CT->IsWeak ? "true" : "false";

    return OK;
}

/* Read-only properties derived from data filelds follow below... */

class TIdProperty : public TProperty {
public:
    TIdProperty() : TProperty(D_ID, EProperty::NONE, "container id (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = fmt::format("{}", CT->Id);
        return OK;
    }
} static IdProperty;

class TLevelProperty : public TProperty {
public:
    TLevelProperty() : TProperty(D_LEVEL, EProperty::NONE, "container level (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = fmt::format("{}", CT->Level);
        return OK;
    }
} static LevelProperty;

class TAbsoluteName : public TProperty {
public:
    TError Get(std::string &value);
    TAbsoluteName() : TProperty(D_ABSOLUTE_NAME, EProperty::NONE,
                                "container name including "
                                "porto namespaces (ro)") {
        IsReadOnly = true;
    }
} static AbsoluteName;

TError TAbsoluteName::Get(std::string &value) {
    if (CT->IsRoot())
        value = ROOT_CONTAINER;
    else
        value = ROOT_PORTO_NAMESPACE + CT->Name;
    return OK;
}

class TAbsoluteNamespace : public TProperty {
public:
    TError Get(std::string &value);
    TAbsoluteNamespace() : TProperty(D_ABSOLUTE_NAMESPACE, EProperty::NONE,
                                     "container namespace "
                                     "including parent "
                                     "namespaces (ro)") {
        IsReadOnly = true;
    }
} static AbsoluteNamespace;

TError TAbsoluteNamespace::Get(std::string &value) {
    value = ROOT_PORTO_NAMESPACE + CT->GetPortoNamespace();
    return OK;
}

class TState : public TProperty {
public:
    TState() : TProperty(D_STATE, EProperty::STATE, "container state (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = TContainer::StateName(CT->State);
        return OK;
    }
} static State;

class TOomKilled : public TProperty {
public:
    TOomKilled() : TProperty(D_OOM_KILLED, EProperty::OOM_KILLED,
                             "container has been killed by OOM (ro)") {
        IsReadOnly = true;
    }
    TError SetFromRestore(const std::string &value) {
        return StringToBool(value, CT->OomKilled);
    }
    TError GetToSave(std::string &value) {
        value = BoolToString(CT->OomKilled);
        return OK;
    }
    TError Get(std::string &value) {
        TError error = IsDead();
        if (!error)
            value = BoolToString(CT->OomKilled);
        return error;
    }
} static OomKilled;

class TOomKills : public TProperty {
public:
    TOomKills() : TProperty(D_OOM_KILLS, EProperty::NONE, "Count of tasks killed in container since start (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        auto cg = MemorySubsystem.RootCgroup();
        uint64_t count;
        IsSupported = !MemorySubsystem.GetOomKills(cg, count);
    }
    TError Get(std::string &value) {
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t count;
        auto error = MemorySubsystem.GetOomKills(cg, count);
        if (!error)
            value = std::to_string(count);
        return error;
    }
} static OomKills;

class TCoreDumped : public TProperty {
public:
    TCoreDumped() : TProperty(D_CORE_DUMPED, EProperty::NONE,
                             "main process dumped core (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsDead();
        if (!error)
            value = BoolToString(WIFSIGNALED(CT->ExitStatus) &&
                                 WCOREDUMP(CT->ExitStatus));
        return error;
    }
} static CoreDumped;

class TOomIsFatal : public TProperty {
public:
    TOomIsFatal() : TProperty(P_OOM_IS_FATAL, EProperty::OOM_IS_FATAL,
                              "Kill all affected containers on OOM (dynamic)") {
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (!error)
            error = StringToBool(value, CT->OomIsFatal);
        if (!error)
            CT->SetProp(EProperty::OOM_IS_FATAL);
        return error;
    }
    TError Get(std::string &value) {
        value = BoolToString(CT->OomIsFatal);
        return OK;
    }
} static OomIsFatal;

class TOomScoreAdj : public TProperty {
public:
    TOomScoreAdj() : TProperty(P_OOM_SCORE_ADJ, EProperty::OOM_SCORE_ADJ,
                               "OOM score adjustment: -1000..1000") {
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        int val;
        error = StringToInt(value, val);
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
    TError Get(std::string &value) {
        value = StringFormat("%d", CT->OomScoreAdj);
        return OK;
    }
} static OomScoreAdj;

class TParent : public TProperty {
public:
    TParent() : TProperty(D_PARENT, EProperty::NONE, "parent container absolute name (ro)") {
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
    TRootPid() : TProperty(D_ROOT_PID, EProperty::NONE, "root task pid (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;

        if (!CT->HasPidFor(*CL->ClientContainer))
            return TError(EError::Permission, "pid is unreachable");

        pid_t pid;
        error = CT->GetPidFor(CL->Pid, pid);
        if (!error)
            value = std::to_string(pid);
        return error;
    }
} static RootPid;

class TExitStatusProperty : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError GetToSave(std::string &value);
    TError Get(std::string &value);
    TExitStatusProperty() : TProperty(D_EXIT_STATUS, EProperty::EXIT_STATUS,
                                      "container exit status (ro)") {
        IsReadOnly = true;
    }
} static ExitStatusProperty;

TError TExitStatusProperty::SetFromRestore(const std::string &value) {
    return StringToInt(value, CT->ExitStatus);
}

TError TExitStatusProperty::GetToSave(std::string &value) {
    value = std::to_string(CT->ExitStatus);

    return OK;
}

TError TExitStatusProperty::Get(std::string &value) {
    TError error = IsDead();
    if (error)
        return error;

    return GetToSave(value);
}

class TExitCodeProperty : public TProperty {
public:
    TExitCodeProperty() : TProperty(D_EXIT_CODE, EProperty::NONE,
            "container exit code, negative: exit signal, OOM: -99 (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        TError error = IsDead();
        if (error)
            return error;
        if (CT->OomKilled)
            value = "-99";
        else if (WIFSIGNALED(CT->ExitStatus))
            value = std::to_string(-WTERMSIG(CT->ExitStatus));
        else
            value = std::to_string(WEXITSTATUS(CT->ExitStatus));
        return OK;
    }
} static ExitCodeProperty;

class TMemUsage : public TProperty {
public:
    TError Get(std::string &value);
    TMemUsage() : TProperty(D_MEMORY_USAGE, EProperty::NONE,
                            "current memory usage [bytes] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
} static MemUsage;

TError TMemUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CT->GetCgroup(MemorySubsystem);
    uint64_t val;
    error = MemorySubsystem.Usage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class TMemReclaimed : public TProperty {
public:
    TMemReclaimed() : TProperty(D_MEMORY_RECLAIMED, EProperty::NONE,
                            "memory reclaimed from container [bytes] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        error = MemorySubsystem.GetReclaimed(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
    }
} static MemReclaimed;

class TAnonUsage : public TProperty {
public:
    TError Get(std::string &value);
    TAnonUsage() : TProperty(D_ANON_USAGE, EProperty::NONE,
                             "current anonymous memory usage [bytes] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
} static AnonUsage;

TError TAnonUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CT->GetCgroup(MemorySubsystem);
    uint64_t val;
    error = MemorySubsystem.GetAnonUsage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class TCacheUsage : public TProperty {
public:
    TCacheUsage() : TProperty(D_CACHE_USAGE, EProperty::NONE,
                            "file cache usage [bytes] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;

        auto cg = CT->GetCgroup(MemorySubsystem);
        uint64_t val;
        error = MemorySubsystem.GetCacheUsage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
    }
} static CacheUsage;

class THugetlbUsage : public TProperty {
public:
    THugetlbUsage() : TProperty(D_HUGETLB_USAGE, EProperty::NONE,
                             "current hugetlb memory usage [bytes] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_HUGETLB;
    }
    void Init(void) {
        IsSupported = HugetlbSubsystem.Supported;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        auto cg = CT->GetCgroup(HugetlbSubsystem);
        uint64_t val;
        error = HugetlbSubsystem.GetHugeUsage(cg, val);
        if (!error)
            value = std::to_string(val);
        return error;
    }
} static HugetlbUsage;

class TMinorFaults : public TProperty {
public:
    TError Get(std::string &value);
    TMinorFaults() : TProperty(D_MINOR_FAULTS, EProperty::NONE, "minor page faults (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
} static MinorFaults;

TError TMinorFaults::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CT->GetCgroup(MemorySubsystem);
    TUintMap stat;

    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_pgfault"] - stat["total_pgmajfault"]);

    return OK;
}

class TMajorFaults : public TProperty {
public:
    TError Get(std::string &value);
    TMajorFaults() : TProperty(D_MAJOR_FAULTS, EProperty::NONE, "major page faults (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
} static MajorFaults;

TError TMajorFaults::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CT->GetCgroup(MemorySubsystem);
    TUintMap stat;

    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_pgmajfault"]);

    return OK;
}

class TMaxRss : public TProperty {
public:
    TError Get(std::string &value);
    TMaxRss() : TProperty(D_MAX_RSS, EProperty::NONE,
                          "peak anonymous memory usage [bytes] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY;
    }
    void Init(void) {
        TCgroup rootCg = MemorySubsystem.RootCgroup();
        TUintMap stat;

        TError error = MemorySubsystem.Statistics(rootCg, stat);
        IsSupported = !error && (stat.find("total_max_rss") != stat.end());
    }
} static MaxRss;

TError TMaxRss::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CT->GetCgroup(MemorySubsystem);
    TUintMap stat;
    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_max_rss"]);

    return OK;
}

class TCpuUsage : public TProperty {
public:
    TError Get(std::string &value);
    TCpuUsage() : TProperty(D_CPU_USAGE, EProperty::NONE, "consumed CPU time [nanoseconds] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_CPUACCT;
    }
} static CpuUsage;

TError TCpuUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CT->GetCgroup(CpuacctSubsystem);
    uint64_t val;
    error = CpuacctSubsystem.Usage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class TCpuSystem : public TProperty {
public:
    TError Get(std::string &value);
    TCpuSystem() : TProperty(D_CPU_SYSTEM, EProperty::NONE,
                             "consumed system CPU time [nanoseconds] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_CPUACCT;
    }
} static CpuSystem;

TError TCpuSystem::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CT->GetCgroup(CpuacctSubsystem);
    uint64_t val;
    error = CpuacctSubsystem.SystemUsage(cg, val);
    if (!error)
        value = std::to_string(val);
    return error;
}

class TCpuWait : public TProperty {
public:
    TCpuWait() : TProperty(D_CPU_WAIT, EProperty::NONE,
                             "CPU time without execution [nanoseconds] (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_CPUACCT;
    }
    void Init(void) {
        IsSupported = CpuacctSubsystem.RootCgroup().Has("cpuacct.wait");
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        auto cg = CT->GetCgroup(CpuacctSubsystem);
        error = cg.Get("cpuacct.wait", value);
        if (!error)
            value = StringTrim(value);
        return error;
    }
} static CpuWait;

class TNetClassId : public TProperty {
public:
    TNetClassId() : TProperty(D_NET_CLASS_ID, EProperty::NONE,
            "network tc class: major:minor (hex) (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (!CT->Net)
            return TError(EError::InvalidState, "not available");
        uint32_t id = CT->NetClass.Leaf;
        auto str = StringFormat("%x:%x", id >> 16, id & 0xFFFF);
        auto lock = CT->Net->LockNet();
        TStringMap map;
        for (auto &dev: CT->Net->Devices)
            if (dev.Managed)
                map[dev.Name] = str;
        value = StringMapToString(map);
        return OK;
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        if (!CT->Net)
            return TError(EError::InvalidState, "not available");
        uint32_t id = CT->NetClass.Leaf;
        auto lock = CT->Net->LockNet();
        for (auto &dev: CT->Net->Devices) {
            if (dev.Managed && dev.Name == index) {
                value = StringFormat("%x:%x", id >> 16, id & 0xFFFF);
                return OK;
            }
        }
        return TError(EError::InvalidProperty, "network device not found");
    }
} NetClassId;

class TNetProperty : public TProperty {
    TUintMap TNetClass:: *Member;
public:
    TNetProperty(std::string name, TUintMap TNetClass:: *member, EProperty prop, std::string desc) :
        TProperty(name, prop, desc), Member(member)
    {
        if (prop == EProperty::NET_PRIO)
            IsHidden = true;
        RequireControllers = CGROUP_NETCLS;
    }
    TError Set(const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;

        error = WantControllers(CGROUP_NETCLS);
        if (error)
            return error;

        TUintMap map;
        error = StringToUintMap(value, map);
        if (error)
            return error;

        auto &cur = CT->NetClass.*Member;
        if (cur != map) {
            CT->SetProp(Prop);
            auto lock = CT->LockNetState();
            /* Host network use same config, take lock if required */
            std::unique_lock<std::mutex> hlock;
            if (!CT->NetClass.HostClass && CT->NetClass.Registered)
                hlock = RootContainer->LockNetState();
            cur = map;
        }

        return OK;
    }

    TError Get(std::string &value) {
        return UintMapToString(CT->NetClass.*Member, value);
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TError error = IsAlive();
        if (error)
            return error;

        uint64_t val;
        error = StringToSize(value, val);
        if (error)
            return TError(EError::InvalidValue, "Invalid value " + value);

        auto &cur = CT->NetClass.*Member;
        if (cur[index] != val) {
            CT->SetProp(Prop);
            auto lock = CT->LockNetState();
            /* Host network use same config, take lock if required */
            std::unique_lock<std::mutex> hlock;
            if (!CT->NetClass.HostClass && CT->NetClass.Registered)
                hlock = RootContainer->LockNetState();
            cur[index] = val;
        }

        return OK;
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        auto lock = CT->LockNetState();
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

TNetProperty NetGuarantee(P_NET_GUARANTEE, &TNetClass::Rate, EProperty::NET_GUARANTEE,
        "Guaranteed network bandwidth: <interface>|default: <Bps>;... (dynamic)");

TNetProperty NetLimit(P_NET_LIMIT, &TNetClass::Limit, EProperty::NET_LIMIT,
        "Maximum network bandwidth: <interface>|default: <Bps>;... (dynamic)");

TNetProperty NetRxLimit(P_NET_RX_LIMIT, &TNetClass::RxLimit, EProperty::NET_RX_LIMIT,
        "Maximum ingress bandwidth: <interface>|default: <Bps>;... (dynamic)");

TNetProperty NetPriority(P_NET_PRIO, &TNetClass::Prio, EProperty::NET_PRIO,
        "Container network priority: <interface>|default: 0-7;... (dynamic)");

class TNetStatProperty : public TProperty {
public:
    uint64_t TNetStat:: *Member;

    TNetStatProperty(std::string name, uint64_t TNetStat:: *member,
                     std::string desc) : TProperty(name, EProperty::NONE, desc) {
        Member = member;
        IsReadOnly = true;
    }

    TError Has() {
        if (Member == &TNetStat::Bytes || Member == &TNetStat::Packets ||
                Member == &TNetStat::Drops || Member == &TNetStat::Overlimits) {
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
        TError error = IsRunning();
        if (error)
            return error;

        TUintMap stat;

        auto lock = CT->LockNetState();
        for (auto &it : CT->NetClass.Fold->Stat)
            stat[it.first] = &it.second->*Member;

        return UintMapToString(stat, value);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;

        auto lock = CT->LockNetState();
        auto it = CT->NetClass.Fold->Stat.find(index);
        if (it == CT->NetClass.Fold->Stat.end())
            return TError(EError::InvalidValue, "network device " + index + " not found");
        value = std::to_string(it->second.*Member);

        return OK;
    }
};

TNetStatProperty NetBytes(D_NET_BYTES, &TNetStat::Bytes,
        "tx bytes: <interface>: <bytes>;... (ro)");
TNetStatProperty NetPackets(D_NET_PACKETS, &TNetStat::Packets,
        "tx packets: <interface>: <packets>;... (ro)");
TNetStatProperty NetDrops(D_NET_DROPS, &TNetStat::Drops,
        "tx drops: <interface>: <packets>;... (ro)");
TNetStatProperty NetOverlimits(D_NET_OVERLIMITS, &TNetStat::Overlimits,
        "tx overlimits: <interface>: <packets>;... (ro)");

TNetStatProperty NetRxBytes(D_NET_RX_BYTES, &TNetStat::RxBytes,
        "device rx bytes: <interface>: <bytes>;... (ro)");
TNetStatProperty NetRxPackets(D_NET_RX_PACKETS, &TNetStat::RxPackets,
        "device rx packets: <interface>: <packets>;... (ro)");
TNetStatProperty NetRxDrops(D_NET_RX_DROPS, &TNetStat::RxDrops,
        "device rx drops: <interface>: <packets>;... (ro)");

TNetStatProperty NetTxBytes(D_NET_TX_BYTES, &TNetStat::TxBytes,
        "device tx bytes: <interface>: <bytes>;... (ro)");
TNetStatProperty NetTxPackets(D_NET_TX_PACKETS, &TNetStat::TxPackets,
        "device tx packets: <interface>: <packets>;... (ro)");
TNetStatProperty NetTxDrops(D_NET_TX_DROPS, &TNetStat::TxDrops,
        "device tx drops: <interface>: <packets>;... (ro)");

class TIoStat : public TProperty {
public:
    TIoStat(std::string name, EProperty prop, std::string desc) : TProperty(name, prop, desc) {
        IsReadOnly = true;
        RequireControllers = CGROUP_MEMORY | CGROUP_BLKIO;
    }
    virtual TError GetMap(TUintMap &map) = 0;
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        TUintMap map;
        error = GetMap(map);
        if (error)
            return error;
        return UintMapToString(map, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TUintMap map;
        TError error;

        error = IsRunning();
        if (error)
            return error;

        error = GetMap(map);
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
    TIoReadStat() : TIoStat(D_IO_READ, EProperty::NONE,
            "read from disk: fs|hw|<disk>|<path>: <bytes>;... (ro)") {}
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
    TIoWriteStat() : TIoStat(D_IO_WRITE, EProperty::NONE,
            "written to disk: fs|hw|<disk>|<path>: <bytes>;... (ro)") {}
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
    TIoOpsStat() : TIoStat(D_IO_OPS, EProperty::NONE,
            "io operations: fs|hw|<disk>|<path>: <ops>;... (ro)") {}
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
    TIoTimeStat() : TIoStat(D_IO_TIME, EProperty::NONE,
            "io time: hw|<disk>|<path>: <nanoseconds>;... (ro)") {}
    TError GetMap(TUintMap &map) {
        auto blkCg = CT->GetCgroup(BlkioSubsystem);
        BlkioSubsystem.GetIoStat(blkCg, TBlkioSubsystem::IoStat::Time, map);
        return OK;
    }
} static IoTimeStat;

class TTime : public TProperty {
public:
    TError Get(std::string &value);
    TTime() : TProperty(D_TIME, EProperty::NONE, "running time [seconds] (ro)") {
        IsReadOnly = true;
    }
} static Time;

TError TTime::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    if (CT->IsRoot()) {
        struct sysinfo si;
        int ret = sysinfo(&si);
        if (ret)
            value = "-1";
        else
            value = std::to_string(si.uptime);

        return OK;
    }

    if (!CT->HasProp(EProperty::DEATH_TIME) &&
        (CT->State == EContainerState::Dead)) {

        CT->DeathTime = GetCurrentTimeMs();
        CT->SetProp(EProperty::DEATH_TIME);
    }

    if (CT->State == EContainerState::Dead)
        value = std::to_string((CT->DeathTime -
                               CT->StartTime) / 1000);
    else
        value = std::to_string((GetCurrentTimeMs() -
                               CT->StartTime) / 1000);

    return OK;
}

class TCreationTime : public TProperty {
public:
    TCreationTime() : TProperty(D_CREATION_TIME, EProperty::NONE, "creation time (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        value = FormatTime(CT->RealCreationTime);
        return OK;
    }
} static CreationTime;

class TStartTime : public TProperty {
public:
    TStartTime() : TProperty(D_START_TIME, EProperty::NONE, "start time (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        if (CT->RealStartTime)
            value = FormatTime(CT->RealStartTime);
        return OK;
    }
} static StartTime;

class TPortoStat : public TProperty {
public:
    void Populate(TUintMap &m);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TPortoStat() : TProperty(D_PORTO_STAT, EProperty::NONE, "porto statistics (ro)") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static PortoStat;

void TPortoStat::Populate(TUintMap &m) {
    m["spawned"] = Statistics->Spawned;
    m["errors"] = Statistics->Errors;
    m["warnings"] = Statistics->Warns;
    m["master_uptime"] = (GetCurrentTimeMs() - Statistics->MasterStarted) / 1000;
    m["porto_uptime"] = (GetCurrentTimeMs() - Statistics->PortoStarted) / 1000;
    m["queued_statuses"] = Statistics->QueuedStatuses;
    m["queued_events"] = Statistics->QueuedEvents;
    m["remove_dead"] = Statistics->RemoveDead;
    m["restore_failed"] = Statistics->RestoreFailed;
    uint64_t usage = 0;
    auto cg = MemorySubsystem.Cgroup(PORTO_DAEMON_CGROUP);
    TError error = MemorySubsystem.Usage(cg, usage);
    if (error)
        L_ERR("Can't get memory usage of portod");
    m["memory_usage_mb"] = usage / 1024 / 1024;

    m["epoll_sources"] = Statistics->EpollSources;

    m["log_lines"] = Statistics->LogLines;
    m["log_bytes"] = Statistics->LogBytes;

    m["log_rotate_bytes"] = Statistics->LogRotateBytes;
    m["log_rotate_errors"] = Statistics->LogRotateErrors;

    m["containers"] = Statistics->ContainersCount - NR_SERVICE_CONTAINERS;

    m["containers_created"] = Statistics->ContainersCreated;
    m["containers_started"] = Statistics->ContainersStarted;
    m["containers_failed_start"] = Statistics->ContainersFailedStart;
    m["containers_oom"] = Statistics->ContainersOOM;

    m["running"] = RootContainer->RunningChildren;
    m["running_children"] = CT->RunningChildren;
    m["starting_children"] = CT->StartingChildren;

    m["volumes"] = Statistics->VolumesCount;
    m["volumes_created"] = Statistics->VolumesCreated;
    m["volumes_failed"] = Statistics->VolumesFailed;

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

    m["requests_longer_1s"] = Statistics->RequestsLonger1s;
    m["requests_longer_3s"] = Statistics->RequestsLonger3s;
    m["requests_longer_30s"] = Statistics->RequestsLonger30s;
    m["requests_longer_5m"] = Statistics->RequestsLonger5m;
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

class TNetTos : public TProperty {
public:
    TError Set(const std::string &) {
        return TError(EError::NotSupported, Name + " is not supported");
    }
    TError Get(std::string &) {
        return TError(EError::NotSupported, "Not supported: " + Name);
    }
    TNetTos() : TProperty(P_NET_TOS, EProperty::NET_TOS, "IP TOS") {
        IsHidden = true;
        IsReadOnly = true;
        IsSupported = false;
    }
} static NetTos;

class TMemTotalLimit : public TProperty {
public:
    TMemTotalLimit() : TProperty(D_MEM_TOTAL_LIMIT, EProperty::NONE,
            "Total memory limit for container hierarchy (ro)") {
        IsReadOnly = true;
    }
    TError Get(std::string &value) {
        auto val = CT->GetTotalMemLimit();
        if (val)
            value = std::to_string(val);
        return OK;
    }
} static MemTotalLimit;

class TProcessCount : public TProperty {
public:
    TProcessCount() : TProperty(D_PROCESS_COUNT, EProperty::NONE, "Total process count (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_FREEZER;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        uint64_t count;
        error = CT->GetProcessCount(count);
        if (!error)
            value = std::to_string(count);
        return error;
    }
} static ProcessCount;

class TThreadCount : public TProperty {
public:
    TThreadCount() : TProperty(D_THREAD_COUNT, EProperty::NONE, "Total thread count (ro)") {
        IsReadOnly = true;
        RequireControllers = CGROUP_FREEZER | CGROUP_PIDS;
    }
    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        uint64_t count;
        error = CT->GetThreadCount(count);
        if (!error)
            value = std::to_string(count);
        return error;
    }
} static ThreadCount;

class TThreadLimit : public TProperty {
public:
    TThreadLimit() : TProperty(P_THREAD_LIMIT, EProperty::THREAD_LIMIT, "Limit pid usage (dynamic)") {}
    void Init() {
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
        error = WantControllers(CGROUP_PIDS);
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
                                  "sysctl: value;...") {}

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
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        TStringMap map;
        error = StringToStringMap(value, map);
        if (error)
            return error;
        CT->Sysctl = map;
        CT->SetProp(EProperty::SYSCTL);
        return OK;
    }

    TError SetIndexed(const std::string &index, const std::string &value) {
        TError error;
        if (value == "")
            CT->Sysctl.erase(index);
        else
            CT->Sysctl[index] = value;
        CT->SetProp(EProperty::SYSCTL);
        return OK;
    }

} static SysctlProperty;

void InitContainerProperties(void) {
    for (auto prop: ContainerProperties)
        prop.second->Init();
}
