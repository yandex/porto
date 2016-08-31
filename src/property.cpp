#include "property.hpp"
#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "client.hpp"
#include "container.hpp"
#include "network.hpp"
#include "statistics.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include <sstream>

extern "C" {
#include <sys/sysinfo.h>
}

__thread TContainer *CurrentContainer = nullptr;
std::map<std::string, TProperty*> ContainerProperties;

TProperty::TProperty(std::string name, EProperty prop, std::string desc) {
    Name = name;
    Prop = prop;
    Desc = desc;
    ContainerProperties[name] = this;
}

TError TProperty::Set(const std::string &value) {
    if (IsReadOnly)
        return TError(EError::InvalidValue, "Read-only value: " + Name);
    return TError(EError::NotSupported, "Not implemented: " + Name);
}

TError TProperty::GetIndexed(const std::string &index, std::string &value) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

TError TProperty::SetIndexed(const std::string &index, const std::string &value) {
    return TError(EError::InvalidValue, "Invalid subscript for property");
}

TError TProperty::GetToSave(std::string &value) {
    if (Prop != EProperty::NONE)
        return Get(value);
    return TError(EError::Unknown, "Trying to save non-serializable value");
}

TError TProperty::SetFromRestore(const std::string &value) {
    if (Prop != EProperty::NONE)
        return Set(value);
    return TError(EError::Unknown, "Trying to restore non-serializable value");
}

/*
 * Note for properties:
 * Dead state 2-line check is mandatory for all properties
 * Some properties require to check if the property is supported
 * Some properties may forbid changing it in runtime
 * Of course, some properties can be read-only
 */

TError TProperty::IsAliveAndStopped(void) {
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

TError TProperty::IsAlive(void) {
    auto state = CurrentContainer->GetState();

    if (state == EContainerState::Dead)
        return TError(EError::InvalidState,
                      "Cannot change property while in the dead state");

    return TError::Success();
}

TError TProperty::IsDead(void) {
    auto state = CurrentContainer->GetState();

    if (state != EContainerState::Dead)
        return TError(EError::InvalidState,
                      "Available only in dead state: " + Name);

    return TError::Success();
}

TError TProperty::IsRunning(void) {
    auto state = CurrentContainer->GetState();

    /*
     * This snippet is taken from TContainer::GetProperty.
     * The method name misguides a bit, but may be the semantic
     * of such properties is that we can look at the value in
     * the dead state too...
     */
    if (state == EContainerState::Stopped)
        return TError(EError::InvalidState,
                      "Not available in stopped state: " + Name);

    return TError::Success();
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

        TCapabilities bound;
        if (CurrentClient->IsSuperUser())
            bound = AllCapabilities;
        else if (CurrentContainer->VirtMode == VIRT_MODE_OS)
            bound = OsModeCapabilities;
        else
            bound = SuidCapabilities;

        /* root user can allow any capabilities in own containers */
        if (!CurrentClient->IsSuperUser() ||
                !CurrentContainer->OwnerCred.IsRootUser()) {
            for (auto p = CurrentContainer->GetParent(); p; p = p->GetParent())
                bound.Permitted &= p->CapLimit.Permitted;
        }

        if (limit.Permitted & ~bound.Permitted) {
            limit.Permitted &= ~bound.Permitted;
            return TError(EError::Permission,
                          "Not allowed capability: " + limit.Format() +
                          ", you can set only: " + bound.Format());
        }

        CurrentContainer->CapLimit = limit;
        CurrentContainer->SetProp(EProperty::CAPABILITIES);
        CurrentContainer->SanitizeCapabilities();
        return TError::Success();
    }

    TError Get(std::string &value) {
        value = CurrentContainer->CapLimit.Format();
        return TError::Success();
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
        value = BoolToString((CurrentContainer->CapLimit.Permitted &
                              caps.Permitted) == caps.Permitted);
        return TError::Success();
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
            caps.Permitted = CurrentContainer->CapLimit.Permitted | caps.Permitted;
        else
            caps.Permitted = CurrentContainer->CapLimit.Permitted & ~caps.Permitted;
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

        /* check allowed ambient capabilities */
        TCapabilities limit = CurrentContainer->CapAllowed;
        if (ambient.Permitted & ~limit.Permitted &&
                !CurrentClient->IsSuperUser()) {
            ambient.Permitted &= ~limit.Permitted;
            return TError(EError::Permission,
                          "Not allowed capability: " + ambient.Format() +
                          ", you can set only: " + limit.Format());
        }

        /* try to raise capabilities limit if required */
        limit = CurrentContainer->CapLimit;
        if (ambient.Permitted & ~limit.Permitted) {
            limit.Permitted |= ambient.Permitted;
            error = Capabilities.CommitLimit(limit);
            if (error)
                return error;
        }

        CurrentContainer->CapAmbient = ambient;
        CurrentContainer->SetProp(EProperty::CAPABILITIES_AMBIENT);
        CurrentContainer->SanitizeCapabilities();
        return TError::Success();
    }

    TError Get(std::string &value) {
        value = CurrentContainer->CapAmbient.Format();
        return TError::Success();
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
        value = BoolToString((CurrentContainer->CapAmbient.Permitted &
                              caps.Permitted) == caps.Permitted);
        return TError::Success();
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
            caps.Permitted = CurrentContainer->CapAmbient.Permitted | caps.Permitted;
        else
            caps.Permitted = CurrentContainer->CapAmbient.Permitted & ~caps.Permitted;
        return CommitAmbient(caps);
    }
} static CapabilitiesAmbient;

class TCwd : public TProperty {
public:
    TCwd() : TProperty(P_CWD, EProperty::CWD, "Container working directory") {}
    TError Get(std::string &value) {
        value = CurrentContainer->GetCwd();
        return TError::Success();
    }
    TError Set(const std::string &cwd) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        CurrentContainer->Cwd = cwd;
        CurrentContainer->SetProp(EProperty::CWD);
        return TError::Success();
    }
} static Cwd;

class TUlimit : public TProperty {
    const std::map<std::string,int> nameToIdx = {
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
        { "stack", RLIMIT_STACK },
    };
public:
    TError Set(const std::string &ulimit);
    TError Get(std::string &value);
    TUlimit() : TProperty(P_ULIMIT, EProperty::ULIMIT,
                          "Container resource limits: "
                          "<type> <soft> <hard>; ... (man 2 getrlimit)") {}
} static Ulimit;

TError TUlimit::Set(const std::string &ulimit_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> ulimits;
    SplitEscapedString(ulimit_str, ulimits, ';');

    /*
     * The final copy will be slow, but we don't want
     * to have inconsistent ulimits inside the container...
     */

    std::map<int,struct rlimit> new_limit;
    for (auto &limit : ulimits) {
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

        new_limit[idx].rlim_cur = soft;
        new_limit[idx].rlim_max = hard;
    }

    CurrentContainer->Rlimit = new_limit;
    CurrentContainer->SetProp(EProperty::ULIMIT);

    return TError::Success();
}

TError TUlimit::Get(std::string &value) {
    std::stringstream str;
    bool first = true;

    for (auto limit_elem : nameToIdx) {
        auto value = CurrentContainer->Rlimit.find(limit_elem.second);
        if (value == CurrentContainer->Rlimit.end())
            continue;

        if (first)
            first = false;
        else
            str << ";";

        str << limit_elem.first << " " <<
            std::to_string((*value).second.rlim_cur) << " " <<
            std::to_string((*value).second.rlim_max);
    }

    value = str.str();

    return TError::Success();
}

class TCpuPolicy : public TProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TCpuPolicy() : TProperty(P_CPU_POLICY, EProperty::CPU_POLICY,
                             "CPU policy: rt, normal, idle (dynamic)" ) {}
} static CpuPolicy;

TError TCpuPolicy::Set(const std::string &policy) {
    TError error = IsAlive();
    if (error)
        return error;

    if (policy != "normal" && policy != "rt" && policy != "idle")
        return TError(EError::InvalidValue, "invalid policy");


    if (CurrentContainer->GetState() == EContainerState::Running ||
        CurrentContainer->GetState() == EContainerState::Meta ||
        CurrentContainer->GetState() == EContainerState::Paused) {

        auto cpucg = CurrentContainer->GetCgroup(CpuSubsystem);
        error = CpuSubsystem.SetCpuPolicy(cpucg, policy,
                                          CurrentContainer->CpuGuarantee,
                                          CurrentContainer->CpuLimit);

        if (error) {
            L_ERR() << "Cannot set cpu policy: " << error << std::endl;
            return error;
        }

    }

    CurrentContainer->CpuPolicy = policy;
    CurrentContainer->SetProp(EProperty::CPU_POLICY);

    return TError::Success();
}

TError TCpuPolicy::Get(std::string &value) {
    value = CurrentContainer->CpuPolicy;

    return TError::Success();
}

class TIoPolicy : public TProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TIoPolicy() : TProperty(P_IO_POLICY, EProperty::IO_POLICY,
                            "IO policy: normal, batch (dynamic)") {}
    void Init(void) {
        IsSupported = BlkioSubsystem.SupportPolicy();
    }
} static IoPolicy;

TError TIoPolicy::Set(const std::string &policy) {
    TError error = IsAlive();
    if (error)
        return error;

    if (policy != "normal" && policy != "batch")
        return TError(EError::InvalidValue, "invalid policy");


    if (CurrentContainer->GetState() == EContainerState::Running ||
        CurrentContainer->GetState() == EContainerState::Meta ||
        CurrentContainer->GetState() == EContainerState::Paused) {

        auto blkcg = CurrentContainer->GetCgroup(BlkioSubsystem);
        error = BlkioSubsystem.SetPolicy(blkcg, policy == "batch");

        if (error) {
            L_ERR() << "Can't set " << P_IO_POLICY << ": " << error << std::endl;
            return error;
        }

    }

    CurrentContainer->IoPolicy = policy;
    CurrentContainer->SetProp(EProperty::IO_POLICY);

    return TError::Success();
}

TError TIoPolicy::Get(std::string &value) {
    value = CurrentContainer->IoPolicy;

    return TError::Success();
}

class TUser : public TProperty {
public:
    TUser() : TProperty(P_USER, EProperty::USER, "Start command with given user") {}

    TError Get(std::string &value) {
        value = UserName(CurrentContainer->OwnerCred.Uid);
        return TError::Success();
    }

    TError Set(const std::string &username) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        TCred newCred;
        gid_t oldGid = CurrentContainer->OwnerCred.Gid;
        error = newCred.Load(username);
        if (error) {
            /* super user can set any numeric id */
            if (CurrentClient->IsSuperUser()) {
                newCred.Gid = oldGid;
                error = UserId(username, newCred.Uid);
            }
            if (error)
                return error;
        }

        /* try to preserve current group if possible */
        if (newCred.IsMemberOf(oldGid) ||
                CurrentClient->Cred.IsMemberOf(oldGid) ||
                CurrentClient->IsSuperUser())
            newCred.Gid = oldGid;

        error = CurrentClient->CanControl(newCred);
        if (error)
            return error;

        CurrentContainer->OwnerCred = newCred;
        CurrentContainer->SetProp(EProperty::USER);
        CurrentContainer->SanitizeCapabilities();
        return TError::Success();
    }
} static User;

class TGroup : public TProperty {
public:
    TGroup() : TProperty(P_GROUP, EProperty::GROUP, "Start command with given group") {}

    TError Get(std::string &value) {
        value = GroupName(CurrentContainer->OwnerCred.Gid);
        return TError::Success();
    }

    TError Set(const std::string &groupname) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        gid_t newGid;
        error = GroupId(groupname, newGid);
        if (error)
            return error;

        if (!CurrentContainer->OwnerCred.IsMemberOf(newGid) &&
                !CurrentClient->Cred.IsMemberOf(newGid) &&
                !CurrentClient->IsSuperUser())
            return TError(EError::Permission, "Desired group : " + groupname +
                    " isn't in current user supplementary group list");

        CurrentContainer->OwnerCred.Gid = newGid;
        CurrentContainer->SetProp(EProperty::GROUP);
        return TError::Success();
    }
} static Group;

class TMemoryGuarantee : public TProperty {
public:
    TError Set(const std::string &mem_guarantee);
    TError Get(std::string &value);
    TMemoryGuarantee() : TProperty(P_MEM_GUARANTEE, EProperty::MEM_GUARANTEE,
                                    "Guaranteed amount of memory "
                                    "[bytes] (dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
} static MemoryGuarantee;

TError TMemoryGuarantee::Set(const std::string &mem_guarantee) {
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
    CurrentContainer->SetProp(EProperty::MEM_GUARANTEE);

    return TError::Success();
}

TError TMemoryGuarantee::Get(std::string &value) {
    value = std::to_string(CurrentContainer->MemGuarantee);

    return TError::Success();
}

class TMemTotalGuarantee : public TProperty {
public:
    TError Get(std::string &value);
    TMemTotalGuarantee() : TProperty(P_MEM_TOTAL_GUARANTEE, EProperty::NONE,
                                     "Total amount of memory "
                                     "guaranteed for porto "
                                     "containers") {
        IsReadOnly = true;
    }
    void Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();
    }
} static MemTotalGuarantee;

TError TMemTotalGuarantee::Get(std::string &value) {
    uint64_t total = CurrentContainer->GetHierarchyMemGuarantee();
    value = std::to_string(total);

    return TError::Success();
}

class TCommand : public TProperty {
public:
    TError Set(const std::string &command);
    TError Get(std::string &value);
    TCommand() : TProperty(P_COMMAND, EProperty::COMMAND,
                           "Command executed upon container start") {}
} static Command;

TError TCommand::Set(const std::string &command) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Command = command;
    CurrentContainer->SetProp(EProperty::COMMAND);

    return TError::Success();
}

TError TCommand::Get(std::string &value) {
    std::string virt_mode;

    value = CurrentContainer->Command;

    return TError::Success();
}

class TVirtMode : public TProperty {
public:
    TError Set(const std::string &virt_mode);
    TError Get(std::string &value);
    TVirtMode() : TProperty(P_VIRT_MODE, EProperty::VIRT_MODE,
                            "Virtualization mode: os|app") {}
} static VirtMode;

TError TVirtMode::Set(const std::string &virt_mode) {
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

    CurrentContainer->SetProp(EProperty::VIRT_MODE);
    CurrentContainer->SanitizeCapabilities();

    return TError::Success();
}

TError TVirtMode::Get(std::string &value) {

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

class TStdinPath : public TProperty {
public:
    TStdinPath() : TProperty(P_STDIN_PATH, EProperty::STDIN,
            "Container standard input path") {}
    TError Get(std::string &value) {
        value = CurrentContainer->Stdin.Path.ToString();
        return TError::Success();
    }
    TError Set(const std::string &path) {
        TError error = IsAliveAndStopped();
        if (!error) {
            CurrentContainer->Stdin.SetInside(path);
            CurrentContainer->SetProp(EProperty::STDIN);
        }
        return error;
    }

} static StdinPath;

class TStdoutPath : public TProperty {
public:
    TStdoutPath() : TProperty(P_STDOUT_PATH, EProperty::STDOUT,
            "Container standard output path") {}
    TError Get(std::string &value) {
        value =  CurrentContainer->Stdout.Path.ToString();
        return TError::Success();
    }
    TError Set(const std::string &path) {
        TError error = IsAliveAndStopped();
        if (!error) {
            CurrentContainer->Stdout.SetInside(path);
            CurrentContainer->SetProp(EProperty::STDOUT);
        }
        return error;
    }
} static StdoutPath;

class TStderrPath : public TProperty {
public:
    TStderrPath() : TProperty(P_STDERR_PATH, EProperty::STDERR,
            "Container standard error path") {}
    TError Get(std::string &value) {
        value = CurrentContainer->Stderr.Path.ToString();
        return TError::Success();
    }
    TError Set(const std::string &path) {
        TError error = IsAliveAndStopped();
        if (!error) {
            CurrentContainer->Stderr.SetInside(path);
            CurrentContainer->SetProp(EProperty::STDERR);
        }
        return error;
    }
} static StderrPath;

class TStdoutLimit : public TProperty {
public:
    TStdoutLimit() : TProperty(P_STDOUT_LIMIT, EProperty::STDOUT_LIMIT,
            "Limit for stored stdout and stderr size (dynamic)") {}
    TError Get(std::string &value) {
        value = std::to_string(CurrentContainer->Stdout.Limit);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        uint64_t limit;
        TError error = StringToSize(value, limit);
        if (error)
            return error;

        auto limit_max = config().container().stdout_limit();
        if (limit > limit_max)
            return TError(EError::InvalidValue,
                    "Maximum limit is: " + std::to_string(limit_max));

        CurrentContainer->Stdout.Limit = limit;
        CurrentContainer->Stderr.Limit = limit;
        CurrentContainer->SetProp(EProperty::STDOUT_LIMIT);
        return TError::Success();
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
        value = std::to_string(CurrentContainer->Stdout.Offset);
        return TError::Success();
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
        value = std::to_string(CurrentContainer->Stderr.Offset);
        return TError::Success();
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
        return CurrentContainer->Stdout.Read(*CurrentContainer, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CurrentContainer->Stdout.Read(*CurrentContainer, value, index);
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
        return CurrentContainer->Stderr.Read(*CurrentContainer, value);
    }
    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        return CurrentContainer->Stderr.Read(*CurrentContainer, value, index);
    }
} static Stderr;

class TBindDns : public TProperty {
public:
    TBindDns() : TProperty(P_BIND_DNS, EProperty::BIND_DNS,
                           "Bind /etc/resolv.conf and /etc/hosts"
                           " from host into container root") {}
    TError Get(std::string &value) {
        value = BoolToString(CurrentContainer->BindDns);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;

        error = StringToBool(value, CurrentContainer->BindDns);
        if (error)
            return error;
        CurrentContainer->SetProp(EProperty::BIND_DNS);
        return TError::Success();
    }
} static BindDns;


class TIsolate : public TProperty {
public:
    TError Set(const std::string &isolate_needed);
    TError Get(std::string &value);
    TIsolate() : TProperty(P_ISOLATE, EProperty::ISOLATE,
                           "Isolate container from parent") {}
} static Isolate;

TError TIsolate::Get(std::string &value) {
    value = CurrentContainer->Isolate ? "true" : "false";

    return TError::Success();
}

TError TIsolate::Set(const std::string &isolate_needed) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (isolate_needed == "true")
        CurrentContainer->Isolate = true;
    else if (isolate_needed == "false")
        CurrentContainer->Isolate = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::ISOLATE);

    return TError::Success();
}

class TRoot : public TProperty {
public:
    TError Set(const std::string &root);
    TError Get(std::string &value);
    TRoot() : TProperty(P_ROOT, EProperty::ROOT, "Container root directory"
                        "(container will be chrooted into ths directory)") {}
} static Root;

TError TRoot::Get(std::string &value) {
    value = CurrentContainer->Root;

    return TError::Success();
}

TError TRoot::Set(const std::string &root) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    CurrentContainer->Root = root;
    CurrentContainer->SetProp(EProperty::ROOT);

    return TError::Success();
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
 "MTU <name> <mtu> | "
 "autoconf <name> (SLAAC) | "
 "netns <name>") {}
} static Net;

TError TNet::Set(const std::string &net_desc) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> new_net_desc;
    SplitEscapedString(net_desc, new_net_desc, ';');

    TNetCfg cfg;
    error = cfg.ParseNet(new_net_desc);
    if (error)
        return error;

    CurrentContainer->NetProp = new_net_desc; /* FIXME: Copy vector contents? */

    CurrentContainer->SetProp(EProperty::NET);
    return TError::Success();
}

TError TNet::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->NetProp, ';');
    return TError::Success();
}

class TRootRo : public TProperty {
public:
    TError Set(const std::string &ro);
    TError Get(std::string &value);
    TRootRo() : TProperty(P_ROOT_RDONLY, EProperty::ROOT_RDONLY,
                          "Mount root directory in read-only mode") {}
} static RootRo;

TError TRootRo::Set(const std::string &ro) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    if (ro == "true")
        CurrentContainer->RootRo = true;
    else if (ro == "false")
        CurrentContainer->RootRo = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::ROOT_RDONLY);

    return TError::Success();
}

TError TRootRo::Get(std::string &ro) {
    ro = CurrentContainer->RootRo ? "true" : "false";

    return TError::Success();
}

class TUmask : public TProperty {
public:
    TUmask() : TProperty(P_UMASK, EProperty::UMASK, "Set file mode creation mask") { }
    TError Get(std::string &value) {
        value = StringFormat("%#o", CurrentContainer->Umask);
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        error = StringToOct(value, CurrentContainer->Umask);
        if (error)
            return error;
        CurrentContainer->SetProp(EProperty::UMASK);
        return TError::Success();
    }
} static Umask;

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

    CurrentContainer->Hostname = hostname;
    CurrentContainer->SetProp(EProperty::HOSTNAME);

    return TError::Success();
}

TError THostname::Get(std::string &value) {
    value = CurrentContainer->Hostname;

    return TError::Success();
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

    std::vector<std::string> envs;
    SplitEscapedString(env_val, envs, ';');

    TEnv env;
    error =  env.Parse(envs, true);
    if (error)
        return error;

    env.Format(CurrentContainer->EnvCfg);
    CurrentContainer->SetProp(EProperty::ENV);

    return TError::Success();
}

TError TEnvProperty::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->EnvCfg, ';');
    return TError::Success();
}

TError TEnvProperty::SetIndexed(const std::string &index, const std::string &env_val) {
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
    CurrentContainer->SetProp(EProperty::ENV);

    return TError::Success();
}

TError TEnvProperty::GetIndexed(const std::string &index, std::string &value) {
    TEnv env;
    TError error = CurrentContainer->GetEnvironment(env);
    if (error)
        return error;

    if (!env.GetEnv(index, value))
        return TError(EError::InvalidValue, "Variable " + index + " not defined");

    return TError::Success();
}

class TBind : public TProperty {
public:
    TError Set(const std::string &bind_str);
    TError Get(std::string &value);
    TBind() : TProperty(P_BIND, EProperty::BIND,
                        "Share host directories with container: "
                        "<host_path> <container_path> [ro|rw]; ...") {}
} static Bind;

TError TBind::Set(const std::string &bind_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> binds;
    SplitEscapedString(bind_str, binds, ';');

    std::vector<TBindMount> bindMounts;

    for (auto &line : binds) {
        std::vector<std::string> tok;
        TBindMount bm;

        SplitEscapedString(line, tok, ' ');
        if (tok.size() != 2 && tok.size() != 3)
            return TError(EError::InvalidValue, "Invalid bind in: " + line);

        bm.Source = tok[0];
        bm.Dest = tok[1];
        bm.ReadOnly = false;
        bm.ReadWrite = false;

        if (tok.size() == 3) {
            if (tok[2] == "ro")
                bm.ReadOnly = true;
            else if (tok[2] == "rw")
                bm.ReadWrite = true;
            else
                return TError(EError::InvalidValue, "Invalid bind type in: " + line);
        }

        bindMounts.push_back(bm);
    }

    CurrentContainer->BindMounts = bindMounts;
    CurrentContainer->SetProp(EProperty::BIND);

    return TError::Success();
}

TError TBind::Get(std::string &value) {
    std::vector<std::string> list;
    for (const auto &bm : CurrentContainer->BindMounts)
        list.push_back(bm.Source.ToString() + " " + bm.Dest.ToString() +
                       (bm.ReadOnly ? " ro" : bm.ReadWrite ? " rw" : ""));
    value = MergeEscapeStrings(list, ';');
    return TError::Success();
}

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

    std::vector<std::string> ipaddrs;
    SplitEscapedString(ipaddr, ipaddrs, ';');

    TNetCfg cfg;
    error = cfg.ParseIp(ipaddrs);
    if (error)
        return error;

    CurrentContainer->IpList = ipaddrs;
    CurrentContainer->SetProp(EProperty::IP);

    return TError::Success();
}

TError TIp::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->IpList, ';');
    return TError::Success();
}

class TDefaultGw : public TProperty {
public:
    TError Set(const std::string &gw);
    TError Get(std::string &value);
    TDefaultGw() : TProperty(P_DEFAULT_GW, EProperty::DEFAULT_GW,
                             "Default gateway: <interface> <ip>; ...") {
        IsHidden = true;
    }
} static DefaultGw;

TError TDefaultGw::Set(const std::string &gw) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    TNetCfg cfg;
    std::vector<std::string> gws;
    SplitEscapedString(gw, gws, ';');

    error = cfg.ParseGw(gws);
    if (error)
        return error;

    CurrentContainer->DefaultGw = gws;
    CurrentContainer->SetProp(EProperty::DEFAULT_GW);

    return TError::Success();
}

TError TDefaultGw::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->DefaultGw, ';');
    return TError::Success();
}

class TResolvConf : public TProperty {
public:
    TError Set(const std::string &conf);
    TError Get(std::string &value);
    TResolvConf() : TProperty(P_RESOLV_CONF, EProperty::RESOLV_CONF,
                              "DNS resolver configuration: "
                              "<resolv.conf option>;...") {}
} static ResolvConf;

TError TResolvConf::Set(const std::string &conf_str) {
    TError error = IsAliveAndStopped();
    if (error)
        return error;

    std::vector<std::string> conf;
    SplitEscapedString(conf_str, conf, ';');

    CurrentContainer->ResolvConf = conf;
    CurrentContainer->SetProp(EProperty::RESOLV_CONF);

    return TError::Success();
}

TError TResolvConf::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->ResolvConf, ';');
    return TError::Success();
}

class TDevices : public TProperty {
public:
    TError Set(const std::string &dev);
    TError Get(std::string &value);
    TDevices() : TProperty(P_DEVICES, EProperty::DEVICES,
                                   "Devices that container can access: "
                                   "<device> [r][w][m][-] [name] [mode] "
                                   "[user] [group]; ...") {}
} static Devices;

TError TDevices::Set(const std::string &dev_str) {
    std::vector<std::string> dev_list;

    SplitEscapedString(dev_str, dev_list, ';');
    CurrentContainer->Devices = dev_list;
    CurrentContainer->SetProp(EProperty::DEVICES);

    return TError::Success();
}

TError TDevices::Get(std::string &value) {
    value = MergeEscapeStrings(CurrentContainer->Devices, ';');
    return TError::Success();
}

class TRawRootPid : public TProperty {
public:
    TRawRootPid() : TProperty(P_RAW_ROOT_PID, EProperty::ROOT_PID, "") {
        IsReadOnly = true;
        IsHidden = true;
    }
    TError Get(std::string &value) {
        value = StringFormat("%d;%d;%d", CurrentContainer->Task.Pid,
                                         CurrentContainer->TaskVPid,
                                         CurrentContainer->WaitTask.Pid);
        return TError::Success();
    }
    TError SetFromRestore(const std::string &value) {
        std::vector<std::string> val;
        TError error;

        SplitEscapedString(value, val, ';');
        if (val.size() > 0)
            error = StringToInt(val[0], CurrentContainer->Task.Pid);
        else
            CurrentContainer->Task.Pid = 0;
        if (!error && val.size() > 1)
            error = StringToInt(val[1], CurrentContainer->TaskVPid);
        else
            CurrentContainer->TaskVPid = 0;
        if (!error && val.size() > 2)
            error = StringToInt(val[2], CurrentContainer->WaitTask.Pid);
        else
            CurrentContainer->WaitTask.Pid = CurrentContainer->Task.Pid;
        return error;
    }
} static RawRootPid;

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
    return StringToInt(value, CurrentContainer->LoopDev);
}

TError TRawLoopDev::Get(std::string &value) {
    value = std::to_string(CurrentContainer->LoopDev);

    return TError::Success();
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
    return StringToUint64(value, CurrentContainer->StartTime);
}

TError TRawStartTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->StartTime);

    return TError::Success();
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
    return StringToUint64(value, CurrentContainer->DeathTime);
}

TError TRawDeathTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->DeathTime);

    return TError::Success();
}

class TPortoNamespace : public TProperty {
public:
    TPortoNamespace() : TProperty(P_PORTO_NAMESPACE, EProperty::PORTO_NAMESPACE,
            "Porto containers namespace (container name prefix)") {}
    TError Get(std::string &value) {
        value = CurrentContainer->NsName;
        return TError::Success();
    }
    TError Set(const std::string &value) {
        TError error = IsAliveAndStopped();
        if (error)
            return error;
        CurrentContainer->NsName = value;
        CurrentContainer->SetProp(EProperty::PORTO_NAMESPACE);
        return TError::Success();
    }
} static PortoNamespace;

class TMemoryLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TMemoryLimit() : TProperty(P_MEM_LIMIT, EProperty::MEM_LIMIT,
                               "Memory hard limit [bytes] (dynamic)") {}
} static MemoryLimit;

TError TMemoryLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (CurrentContainer->MemLimit != new_size) {
        CurrentContainer->MemLimit = new_size;
        CurrentContainer->SetProp(EProperty::MEM_LIMIT);
    }

    return TError::Success();
}

TError TMemoryLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->MemLimit);

    return TError::Success();
}

class TAnonLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TAnonLimit() : TProperty(P_ANON_LIMIT, EProperty::ANON_LIMIT,
                             "Anonymous memory limit [bytes] (dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportAnonLimit();
    }

} static AnonLimit;

TError TAnonLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (CurrentContainer->AnonMemLimit != new_size) {
        CurrentContainer->AnonMemLimit = new_size;
        CurrentContainer->SetProp(EProperty::ANON_LIMIT);
    }

    return TError::Success();
}

TError TAnonLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->AnonMemLimit);

    return TError::Success();
}

class TDirtyLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TDirtyLimit() : TProperty(P_DIRTY_LIMIT, EProperty::DIRTY_LIMIT,
                              "Dirty file cache limit [bytes] "
                              "(dynamic)" ) {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportDirtyLimit();
    }
} static DirtyLimit;

TError TDirtyLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_size = 0lu;
    error = StringToSize(limit, new_size);
    if (error)
        return error;

    if (CurrentContainer->DirtyMemLimit != new_size) {
        CurrentContainer->DirtyMemLimit = new_size;
        CurrentContainer->SetProp(EProperty::ANON_LIMIT);
    }

    return TError::Success();
}

TError TDirtyLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->DirtyMemLimit);

    return TError::Success();
}

class TRechargeOnPgfault : public TProperty {
public:
    TError Set(const std::string &recharge);
    TError Get(std::string &value);
    TRechargeOnPgfault() : TProperty(P_RECHARGE_ON_PGFAULT,
                                     EProperty::RECHARGE_ON_PGFAULT,
                                     "Recharge memory on "
                                     "page fault (dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportRechargeOnPgfault();
    }
} static RechargeOnPgfault;

TError TRechargeOnPgfault::Set(const std::string &recharge) {
    TError error = IsAlive();
    if (error)
        return error;

    bool new_val;
    if (recharge == "true")
        new_val = true;
    else if (recharge == "false")
        new_val = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    if (CurrentContainer->RechargeOnPgfault != new_val) {
        CurrentContainer->RechargeOnPgfault = new_val;
        CurrentContainer->SetProp(EProperty::RECHARGE_ON_PGFAULT);
    }

    return TError::Success();
}

TError TRechargeOnPgfault::Get(std::string &value) {
    value = CurrentContainer->RechargeOnPgfault ? "true" : "false";

    return TError::Success();
}

class TCpuLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TCpuLimit() : TProperty(P_CPU_LIMIT, EProperty::CPU_LIMIT,
                            "CPU limit: 0-100.0 [%] | 0.0c-<CPUS>c "
                            " [cores] (dynamic)") {}
} static CpuLimit;

TError TCpuLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    double new_limit;
    error = StringToCpuValue(limit, new_limit);
    if (error)
        return error;

    if (CurrentContainer->CpuLimit != new_limit) {
        CurrentContainer->CpuLimit = new_limit;
        CurrentContainer->SetProp(EProperty::CPU_LIMIT);
    }

    return TError::Success();
}

TError TCpuLimit::Get(std::string &value) {
    value = StringFormat("%lgc", CurrentContainer->CpuLimit);

    return TError::Success();
}

class TCpuGuarantee : public TProperty {
public:
    TError Set(const std::string &guarantee);
    TError Get(std::string &value);
    TCpuGuarantee() : TProperty(P_CPU_GUARANTEE, EProperty::CPU_GUARANTEE,
                                "CPU guarantee: 0-100.0 [%] | "
                                "0.0c-<CPUS>c [cores] (dynamic)") {}
} static CpuGuarantee;

TError TCpuGuarantee::Set(const std::string &guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    double new_guarantee;
    error = StringToCpuValue(guarantee, new_guarantee);
    if (error)
        return error;

    if (CurrentContainer->CpuGuarantee != new_guarantee) {
        CurrentContainer->CpuGuarantee = new_guarantee;
        CurrentContainer->SetProp(EProperty::CPU_GUARANTEE);
    }

    return TError::Success();
}

TError TCpuGuarantee::Get(std::string &value) {
    value = StringFormat("%lgc", CurrentContainer->CpuGuarantee);

    return TError::Success();
}

class TIoLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TIoLimit()  : TProperty(P_IO_LIMIT, EProperty::IO_LIMIT,
                            "Filesystem bandwidth limit [bytes/s] "
                            "(dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportIoLimit();
    }
} static IoLimit;

TError TIoLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_limit = 0lu;
    error = StringToSize(limit, new_limit);
    if (error)
        return error;

    if (CurrentContainer->IoLimit != new_limit) {
        CurrentContainer->IoLimit = new_limit;
        CurrentContainer->SetProp(EProperty::IO_LIMIT);
    }

    return TError::Success();
}

TError TIoLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->IoLimit);

    return TError::Success();
}

class TIopsLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TIopsLimit() : TProperty(P_IO_OPS_LIMIT, EProperty::IO_OPS_LIMIT,
                             "Filesystem IOPS limit "
                             "[operations/s] (dynamic)") {}
    void Init(void) {
        IsSupported = MemorySubsystem.SupportIoLimit();
    }
} static IopsLimit;

TError TIopsLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_limit = 0lu;
    error = StringToSize(limit, new_limit);
    if (error)
        return error;

    if (CurrentContainer->IopsLimit != new_limit) {
        CurrentContainer->IopsLimit = new_limit;
        CurrentContainer->SetProp(EProperty::IO_OPS_LIMIT);
    }

    return TError::Success();
}

TError TIopsLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->IopsLimit);

    return TError::Success();
}

class TNetGuarantee : public TProperty {
public:
    TError Set(const std::string &guarantee);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &guarantee);
    TError GetIndexed(const std::string &index, std::string &value);
    TNetGuarantee() : TProperty(P_NET_GUARANTEE, EProperty::NET_GUARANTEE,
                                "Guaranteed container network "
                                "bandwidth: <interface>|default "
                                "<Bps>;... (dynamic)") {}
} static NetGuarantee;

TError TNetGuarantee::Set(const std::string &guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    TUintMap new_guarantee;
    error = StringToUintMap(guarantee, new_guarantee);
    if (error)
        return error;

    if (CurrentContainer->NetGuarantee != new_guarantee) {
        CurrentContainer->NetGuarantee = new_guarantee;
        CurrentContainer->SetProp(EProperty::NET_GUARANTEE);
    }

    return TError::Success();
}

TError TNetGuarantee::Get(std::string &value) {
    return UintMapToString(CurrentContainer->NetGuarantee, value);
}

TError TNetGuarantee::SetIndexed(const std::string &index,
                                          const std::string &guarantee) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t val;
    error = StringToSize(guarantee, val);
    if (error)
        return TError(EError::InvalidValue, "Invalid value " + guarantee);

    if (CurrentContainer->NetGuarantee[index] != val) {
        CurrentContainer->NetGuarantee[index] = val;
        CurrentContainer->SetProp(EProperty::NET_GUARANTEE);
    }

    return TError::Success();
}

TError TNetGuarantee::GetIndexed(const std::string &index,
                                          std::string &value) {

    if (CurrentContainer->NetGuarantee.find(index) ==
        CurrentContainer->NetGuarantee.end())

        return TError(EError::InvalidValue, "invalid index " + index);

    value = std::to_string(CurrentContainer->NetGuarantee[index]);

    return TError::Success();
}

class TNetLimit : public TProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &limit);
    TError GetIndexed(const std::string &index, std::string &value);
    TNetLimit() : TProperty(P_NET_LIMIT, EProperty::NET_LIMIT,
                            "Maximum container network bandwidth: "
                            "<interface>|default <Bps>;... (dynamic)") {}
} static NetLimit;

TError TNetLimit::Set(const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    TUintMap new_limit;
    error = StringToUintMap(limit, new_limit);
    if (error)
        return error;

    if (CurrentContainer->NetLimit != new_limit) {
        CurrentContainer->NetLimit = new_limit;
        CurrentContainer->SetProp(EProperty::NET_LIMIT);
    }

    return TError::Success();
}

TError TNetLimit::Get(std::string &value) {
    return UintMapToString(CurrentContainer->NetLimit, value);
}

TError TNetLimit::SetIndexed(const std::string &index,
                                      const std::string &limit) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t val;
    error = StringToSize(limit, val);
    if (error)
        return TError(EError::InvalidValue, "Invalid value " + limit);

    if (CurrentContainer->NetLimit[index] != val) {
        CurrentContainer->NetLimit[index] = val;
        CurrentContainer->SetProp(EProperty::NET_LIMIT);
    }

    return TError::Success();
}

TError TNetLimit::GetIndexed(const std::string &index,
                                      std::string &value) {

    if (CurrentContainer->NetLimit.find(index) ==
        CurrentContainer->NetLimit.end())

        return TError(EError::InvalidValue, "invalid index " + index);

    value = std::to_string(CurrentContainer->NetLimit[index]);

    return TError::Success();
}

class TNetPriority : public TProperty {
public:
    TError Set(const std::string &prio);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &prio);
    TError GetIndexed(const std::string &index, std::string &value);
    TNetPriority()  : TProperty(P_NET_PRIO, EProperty::NET_PRIO,
                                "Container network priority: "
                                "<interface>|default 0-7;... "
                                "(dynamic)") {}
} static NetPriority;

TError TNetPriority::Set(const std::string &prio) {
    TError error = IsAlive();
    if (error)
        return error;

    TUintMap new_prio;
    error = StringToUintMap(prio, new_prio);
    if (error)
        return error;

    for (auto &kv : new_prio) {
        if (kv.second > 7)
            return TError(EError::InvalidValue, "invalid value");
    }

    if (CurrentContainer->NetPriority != new_prio) {
        CurrentContainer->NetPriority = new_prio;
        CurrentContainer->SetProp(EProperty::NET_PRIO);
    }

    return TError::Success();
}

TError TNetPriority::Get(std::string &value) {
    return UintMapToString(CurrentContainer->NetPriority, value);
}

TError TNetPriority::SetIndexed(const std::string &index,
                                      const std::string &prio) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t val;
    error = StringToSize(prio, val);
    if (error)
        return TError(EError::InvalidValue, "Invalid value " + prio);

    if (val > 7)
        return TError(EError::InvalidValue, "invalid value");

    if (CurrentContainer->NetPriority[index] != val) {
        CurrentContainer->NetPriority[index] = val;
        CurrentContainer->SetProp(EProperty::NET_PRIO);
    }

    return TError::Success();
}

TError TNetPriority::GetIndexed(const std::string &index,
                                      std::string &value) {

    if (CurrentContainer->NetPriority.find(index) ==
        CurrentContainer->NetPriority.end())

        return TError(EError::InvalidValue, "invalid index " + index);

    value = std::to_string(CurrentContainer->NetPriority[index]);

    return TError::Success();
}

class TRespawn : public TProperty {
public:
    TError Set(const std::string &respawn);
    TError Get(std::string &value);
    TRespawn() : TProperty(P_RESPAWN, EProperty::RESPAWN,
                           "Automatically respawn dead container (dynamic)") {}
} static Respawn;

TError TRespawn::Set(const std::string &respawn) {
    TError error = IsAlive();
    if (error)
        return error;

    if (respawn == "true")
        CurrentContainer->ToRespawn = true;
    else if (respawn == "false")
        CurrentContainer->ToRespawn = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::RESPAWN);

    return TError::Success();
}

TError TRespawn::Get(std::string &value) {
    value = CurrentContainer->ToRespawn ? "true" : "false";

    return TError::Success();
}

class TMaxRespawns : public TProperty {
public:
    TError Set(const std::string &max);
    TError Get(std::string &value);
    TMaxRespawns() : TProperty(P_MAX_RESPAWNS, EProperty::MAX_RESPAWNS,
                               "Limit respawn count for specific "
                               "container (dynamic)") {}
} static MaxRespawns;

TError TMaxRespawns::Set(const std::string &max) {
    TError error = IsAlive();
    if (error)
        return error;

    int new_value;
    if (StringToInt(max, new_value))
        return TError(EError::InvalidValue, "Invalid integer value " + max);

    CurrentContainer->MaxRespawns = new_value;
    CurrentContainer->SetProp(EProperty::MAX_RESPAWNS);

    return TError::Success();
}

TError TMaxRespawns::Get(std::string &value) {
    value = std::to_string(CurrentContainer->MaxRespawns);

    return TError::Success();
}

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

    CurrentContainer->Private = value;

    return TError::Success();
}

TError TPrivate::Get(std::string &value) {
    value = CurrentContainer->Private;

    return TError::Success();
}

class TAgingTime : public TProperty {
public:
    TError Set(const std::string &time);
    TError Get(std::string &value);
    TAgingTime() : TProperty(P_AGING_TIME, EProperty::AGING_TIME,
                             "After given number of seconds "
                             "container in dead state is "
                             "automatically removed (dynamic)") {}
} static AgingTime;

TError TAgingTime::Set(const std::string &time) {
    TError error = IsAlive();
    if (error)
        return error;

    uint64_t new_time;
    error = StringToUint64(time, new_time);
    if (error)
        return error;

    CurrentContainer->AgingTime = new_time;
    CurrentContainer->SetProp(EProperty::AGING_TIME);

    return TError::Success();
}

TError TAgingTime::Get(std::string &value) {
    value = std::to_string(CurrentContainer->AgingTime);

    return TError::Success();
}

class TEnablePorto : public TProperty {
public:
    TEnablePorto() : TProperty(P_ENABLE_PORTO, EProperty::ENABLE_PORTO,
            "Proto access level: false | read-only | child-only | true (dynamic)") {}
    TError Get(std::string &value) {
        switch (CurrentContainer->AccessLevel) {
            case EAccessLevel::None:
                value = "false";
                break;
            case EAccessLevel::ReadOnly:
                value = "read-only";
                break;
            case EAccessLevel::ChildOnly:
                value = "child-only";
                break;
            default:
                value = "true";
                break;
        }
        return TError::Success();
    }
    TError Set(const std::string &value) {
        EAccessLevel level;

        if (value == "false")
            level = EAccessLevel::None;
        else if (value == "read-only")
            level = EAccessLevel::ReadOnly;
        else if (value == "child-only")
            level = EAccessLevel::ChildOnly;
        else if (value == "true")
            level = EAccessLevel::Normal;
        else
            return TError(EError::InvalidValue, "Unknown access level: " + value);

        if (level > EAccessLevel::ChildOnly) {
            for (auto p = CurrentContainer->Parent; p; p = p->Parent)
                if (p->AccessLevel < EAccessLevel::ChildOnly)
                    return TError(EError::Permission,
                            "Parent container has access lower than child");
        }

        CurrentContainer->AccessLevel = level;
        CurrentContainer->SetProp(EProperty::ENABLE_PORTO);
        return TError::Success();
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
        CurrentContainer->IsWeak = true;
    else if (weak == "false")
        CurrentContainer->IsWeak = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    CurrentContainer->SetProp(EProperty::WEAK);

    return TError::Success();
}

TError TWeak::Get(std::string &value) {
    value = CurrentContainer->IsWeak ? "true" : "false";

    return TError::Success();
}

/* Read-only properties derived from data filelds follow below... */

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
    if (CurrentContainer->IsRoot() || CurrentContainer->IsPortoRoot())
        value = CurrentContainer->GetName();
    else
        value = std::string(PORTO_ROOT_CONTAINER) + "/" +
                CurrentContainer->GetName();

    return TError::Success();
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
    value = std::string(PORTO_ROOT_CONTAINER) + "/" +
            CurrentContainer->GetPortoNamespace();

    return TError::Success();
}

class TState : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TState() : TProperty(D_STATE, EProperty::STATE, "container state (ro)") {
        IsReadOnly = true;
    }
} static State;

TError TState::SetFromRestore(const std::string &value) {
    /*
     * We are just restoring value indication there.
     * The container must manually call SetState()
     * TContainer::Restore() handler.
     */

    if (value == "stopped")
        CurrentContainer->State = EContainerState::Stopped;
    else if (value == "dead")
        CurrentContainer->State = EContainerState::Dead;
    else if (value == "running")
        CurrentContainer->State = EContainerState::Running;
    else if (value == "paused")
        CurrentContainer->State = EContainerState::Paused;
    else if (value == "meta")
        CurrentContainer->State = EContainerState::Meta;
    else if (value == "unknown")
        CurrentContainer->State  = EContainerState::Unknown;
    else
        return TError(EError::Unknown, "Invalid container saved state");

    return TError::Success();
}

TError TState::Get(std::string &value) {
    value = CurrentContainer->ContainerStateName(CurrentContainer->GetState());

    return TError::Success();
}

class TOomKilled : public TProperty {
public:
    TOomKilled() : TProperty(D_OOM_KILLED, EProperty::OOM_KILLED,
                             "container has been killed by OOM (ro)") {
        IsReadOnly = true;
    }
    TError SetFromRestore(const std::string &value) {
        return StringToBool(value, CurrentContainer->OomKilled);
    }
    TError GetToSave(std::string &value) {
        value = BoolToString(CurrentContainer->OomKilled);
        return TError::Success();
    }
    TError Get(std::string &value) {
        TError error = IsDead();
        if (!error)
            value = BoolToString(CurrentContainer->OomKilled);
        return error;
    }
} static OomKilled;

class TParent : public TProperty {
public:
    TError Get(std::string &value);
    TParent() : TProperty(D_PARENT, EProperty::NONE,
                          "parent container name (ro) (deprecated)") {
        IsReadOnly = true;
        IsHidden = true;
    }
} static Parent;

TError TParent::Get(std::string &value) {
    auto p = CurrentContainer->GetParent();
    value = p ? p->GetName() : "";

    return TError::Success();
}

class TRespawnCount : public TProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TRespawnCount() : TProperty(D_RESPAWN_COUNT, EProperty::RESPAWN_COUNT,
                                "current respawn count (ro)") {
        IsReadOnly = true;
    }
} static RespawnCount;

TError TRespawnCount::SetFromRestore(const std::string &value) {
    return StringToUint64(value, CurrentContainer->RespawnCount);
}

TError TRespawnCount::Get(std::string &value) {
    value = std::to_string(CurrentContainer->RespawnCount);

    return TError::Success();
}

class TRootPid : public TProperty {
public:
    TRootPid() : TProperty(D_ROOT_PID, EProperty::NONE,
            "root task pid (ro)") {
        IsHidden = true;
    }

    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        value = std::to_string(CurrentContainer->GetPidFor(CurrentClient->Pid));
        return TError::Success();
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
    return StringToInt(value, CurrentContainer->ExitStatus);
}

TError TExitStatusProperty::GetToSave(std::string &value) {
    value = std::to_string(CurrentContainer->ExitStatus);

    return TError::Success();
}

TError TExitStatusProperty::Get(std::string &value) {
    TError error = IsDead();
    if (error)
        return error;

    return GetToSave(value);
}

class TMemUsage : public TProperty {
public:
    TError Get(std::string &value);
    TMemUsage() : TProperty(D_MEMORY_USAGE, EProperty::NONE,
                            "current memory usage [bytes] (ro)") {
        IsReadOnly = true;
    }
} static MemUsage;

TError TMemUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);

    uint64_t val;
    error = MemorySubsystem.Usage(cg, val);
    if (error) {
        L_ERR() << "Can't get memory usage: " << error << std::endl;
        return error;
    }

    value = std::to_string(val);

    return TError::Success();
}

class TAnonUsage : public TProperty {
public:
    TError Get(std::string &value);
    TAnonUsage() : TProperty(D_ANON_USAGE, EProperty::NONE,
                             "current anonymous memory usage [bytes] (ro)") {
        IsReadOnly = true;
    }
} static AnonUsage;

TError TAnonUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    uint64_t val;

    if (MemorySubsystem.GetAnonUsage(cg, val))
        value = "0";
    else
        value = std::to_string(val);

    return TError::Success();
}

class TMinorFaults : public TProperty {
public:
    TError Get(std::string &value);
    TMinorFaults() : TProperty(D_MINOR_FAULTS, EProperty::NONE, "minor page faults (ro)") {
        IsReadOnly = true;
    }
} static MinorFaults;

TError TMinorFaults::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    TUintMap stat;

    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_pgfault"] - stat["total_pgmajfault"]);

    return TError::Success();
}

class TMajorFaults : public TProperty {
public:
    TError Get(std::string &value);
    TMajorFaults() : TProperty(D_MAJOR_FAULTS, EProperty::NONE, "major page faults (ro)") {
        IsReadOnly = true;
    }
} static MajorFaults;

TError TMajorFaults::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    TUintMap stat;

    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_pgmajfault"]);

    return TError::Success();
}

class TMaxRss : public TProperty {
public:
    TError Get(std::string &value);
    TMaxRss() : TProperty(D_MAX_RSS, EProperty::NONE,
                          "peak anonymous memory usage [bytes] (ro)") {
        IsReadOnly = true;
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

    auto cg = CurrentContainer->GetCgroup(MemorySubsystem);
    TUintMap stat;
    if (MemorySubsystem.Statistics(cg, stat))
        value = "-1";
    else
        value = std::to_string(stat["total_max_rss"]);

    return TError::Success();
}

class TCpuUsage : public TProperty {
public:
    TError Get(std::string &value);
    TCpuUsage() : TProperty(D_CPU_USAGE, EProperty::NONE, "consumed CPU time [nanoseconds] (ro)") {
        IsReadOnly = true;
    }
} static CpuUsage;

TError TCpuUsage::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(CpuacctSubsystem);

    uint64_t val;
    error = CpuacctSubsystem.Usage(cg, val);

    if (error) {
        L_ERR() << "Can't get CPU usage: " << error << std::endl;
        value = "-1";
    } else {
        value = std::to_string(val);
    }

    return TError::Success();
}

class TCpuSystem : public TProperty {
public:
    TError Get(std::string &value);
    TCpuSystem() : TProperty(D_CPU_SYSTEM, EProperty::NONE,
                             "consumed system CPU time [nanoseconds] (ro)") {
        IsReadOnly = true;
    }
} static CpuSystem;

TError TCpuSystem::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    auto cg = CurrentContainer->GetCgroup(CpuacctSubsystem);

    uint64_t val;
    error = CpuacctSubsystem.SystemUsage(cg, val);

    if (error) {
        L_ERR() << "Can't get system CPU usage: " << error << std::endl;
        value = "-1";
    } else {
        value = std::to_string(val);
    }

    return TError::Success();
}

class TNetStat : public TProperty {
public:
    ENetStat Kind;

    TNetStat(std::string name, ENetStat kind, std::string desc) :
            TProperty(name, EProperty::NONE, desc) {
        Kind = kind;
        IsReadOnly = true;
    }

    TError Get(std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        TUintMap stat;
        error = CurrentContainer->GetNetStat(Kind, stat);
        if (error)
            return error;
        return UintMapToString(stat, value);
    }

    TError GetIndexed(const std::string &index, std::string &value) {
        TError error = IsRunning();
        if (error)
            return error;
        TUintMap stat;
        error = CurrentContainer->GetNetStat(Kind, stat);
        if (error)
            return error;
        if (stat.find(index) == stat.end())
            return TError(EError::InvalidValue, "network device " + index + " no found");
        value = std::to_string(stat[index]);
        return TError::Success();
    }
};

TNetStat NetBytes(D_NET_BYTES, ENetStat::Bytes, "tx bytes: <interface>: <bytes>;... (ro)");
TNetStat NetPackets(D_NET_PACKETS, ENetStat::Packets, "tx packets: <interface>: <packets>;... (ro)");
TNetStat NetDrops(D_NET_DROPS, ENetStat::Drops, "tx drops: <interface>: <packets>;... (ro)");
TNetStat NetOverlimits(D_NET_OVERLIMITS, ENetStat::Overlimits, "tx overlimits: <interface>: <packets>;... (ro)");

TNetStat NetRxBytes(D_NET_RX_BYTES, ENetStat::RxBytes, "device rx bytes: <interface>: <bytes>;... (ro)");
TNetStat NetRxPackets(D_NET_RX_PACKETS, ENetStat::RxPackets, "device rx packets: <interface>: <packets>;... (ro)");
TNetStat NetRxDrops(D_NET_RX_DROPS, ENetStat::RxDrops, "device rx drops: <interface>: <packets>;... (ro)");

TNetStat NetTxBytes(D_NET_TX_BYTES, ENetStat::TxBytes, "device tx bytes: <interface>: <bytes>;... (ro)");
TNetStat NetTxPackets(D_NET_TX_PACKETS, ENetStat::TxPackets, "device tx packets: <interface>: <packets>;... (ro)");
TNetStat NetTxDrops(D_NET_TX_DROPS, ENetStat::TxDrops, "device tx drops: <interface>: <packets>;... (ro)");

class TIoRead : public TProperty {
public:
    void Populate(TUintMap &m);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TIoRead() : TProperty(D_IO_READ, EProperty::NONE, "read from disk [bytes] (ro)") {
        IsReadOnly = true;
    }
} static IoRead;

void TIoRead::Populate(TUintMap &m) {
    auto memCg = CurrentContainer->GetCgroup(MemorySubsystem);
    auto blkCg = CurrentContainer->GetCgroup(BlkioSubsystem);
    TUintMap memStat;

    TError error = MemorySubsystem.Statistics(memCg, memStat);
    if (!error)
        m["fs"] = memStat["fs_io_bytes"] - memStat["fs_io_write_bytes"];

    std::vector<BlkioStat> blkStat;
    error = BlkioSubsystem.Statistics(blkCg, "blkio.io_service_bytes_recursive", blkStat);
    if (!error) {
        for (auto &s : blkStat)
            m[s.Device] = s.Read;
    }
}

TError TIoRead::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    TUintMap m;
    Populate(m);

    return UintMapToString(m, value);
}

TError TIoRead::GetIndexed(const std::string &index,
                                    std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    TUintMap m;
    Populate(m);

    if (m.find(index) == m.end())
        return TError(EError::InvalidValue, "Invalid subscript for property");

    value = std::to_string(m[index]);

    return TError::Success();
}

class TIoWrite : public TProperty {
public:
    void Populate(TUintMap &m);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TIoWrite() : TProperty(D_IO_WRITE, EProperty::NONE, "written to disk [bytes] (ro)") {
        IsReadOnly = true;
    }
} static IoWrite;

void TIoWrite::Populate(TUintMap &m) {
    auto memCg = CurrentContainer->GetCgroup(MemorySubsystem);
    auto blkCg = CurrentContainer->GetCgroup(BlkioSubsystem);
    TUintMap memStat;

    TError error = MemorySubsystem.Statistics(memCg, memStat);
    if (!error)
        m["fs"] = memStat["fs_io_write_bytes"];

    std::vector<BlkioStat> blkStat;
    error = BlkioSubsystem.Statistics(blkCg, "blkio.io_service_bytes_recursive", blkStat);
    if (!error) {
        for (auto &s : blkStat)
            m[s.Device] = s.Write;
    }

}

TError TIoWrite::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    TUintMap m;
    Populate(m);

    return UintMapToString(m, value);
}

TError TIoWrite::GetIndexed(const std::string &index, std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    TUintMap m;
    Populate(m);

    if (m.find(index) == m.end())
        return TError(EError::InvalidValue, "Invalid subscript for property");

    value = std::to_string(m[index]);

    return TError::Success();
}

class TIoOps : public TProperty {
public:
    void Populate(TUintMap &m);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TIoOps() : TProperty(D_IO_OPS, EProperty::NONE, "io operations (ro)") {
        IsReadOnly = true;
    }
} static IoOps;

void TIoOps::Populate(TUintMap &m) {
    auto memCg = CurrentContainer->GetCgroup(MemorySubsystem);
    auto blkCg = CurrentContainer->GetCgroup(BlkioSubsystem);
    TUintMap memStat;

    TError error = MemorySubsystem.Statistics(memCg, memStat);
    if (!error)
        m["fs"] = memStat["fs_io_operations"];

    std::vector<BlkioStat> blkStat;
    error = BlkioSubsystem.Statistics(blkCg, "blkio.io_service_bytes_recursive", blkStat);
    if (!error) {
        for (auto &s : blkStat)
            m[s.Device] = s.Read + s.Write;
    }
}

TError TIoOps::Get(std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    TUintMap m;
    Populate(m);

    return UintMapToString(m, value);
}

TError TIoOps::GetIndexed(const std::string &index,
                                   std::string &value) {
    TError error = IsRunning();
    if (error)
        return error;

    TUintMap m;
    Populate(m);

    if (m.find(index) == m.end())
        return TError(EError::InvalidValue, "Invalid subscript for property");

    value = std::to_string(m[index]);

    return TError::Success();
}

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

    if (CurrentContainer->IsRoot()) {
        struct sysinfo si;
        int ret = sysinfo(&si);
        if (ret)
            value = "-1";
        else
            value = std::to_string(si.uptime);

        return TError::Success();
    }

    // we started recording raw start/death time since porto v1.15;
    // in case we updated from old version, return zero
    if (!CurrentContainer->HasProp(EProperty::START_TIME)) {
        CurrentContainer->StartTime = GetCurrentTimeMs();
        CurrentContainer->SetProp(EProperty::START_TIME);
    }

    if (!CurrentContainer->HasProp(EProperty::DEATH_TIME) &&
        (CurrentContainer->GetState() == EContainerState::Dead)) {

        CurrentContainer->DeathTime = GetCurrentTimeMs();
        CurrentContainer->SetProp(EProperty::DEATH_TIME);
    }

    if (CurrentContainer->GetState() == EContainerState::Dead)
        value = std::to_string((CurrentContainer->DeathTime -
                               CurrentContainer->StartTime) / 1000);
    else
        value = std::to_string((GetCurrentTimeMs() -
                               CurrentContainer->StartTime) / 1000);

    return TError::Success();
}

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
    m["slave_uptime"] = (GetCurrentTimeMs() - Statistics->SlaveStarted) / 1000;
    m["queued_statuses"] = Statistics->QueuedStatuses;
    m["queued_events"] = Statistics->QueuedEvents;
    m["created"] = Statistics->Created;
    m["remove_dead"] = Statistics->RemoveDead;
    m["slave_timeout_ms"] = Statistics->SlaveTimeoutMs;
    m["rotated"] = Statistics->Rotated;
    m["restore_failed"] = Statistics->RestoreFailed;
    m["started"] = Statistics->Started;
    m["running"] = CurrentContainer->GetRunningChildren();
    uint64_t usage = 0;
    auto cg = MemorySubsystem.Cgroup(PORTO_DAEMON_CGROUP);
    TError error = MemorySubsystem.Usage(cg, usage);
    if (error)
        L_ERR() << "Can't get memory usage of portod" << std::endl;
    m["memory_usage_mb"] = usage / 1024 / 1024;
    m["epoll_sources"] = Statistics->EpollSources;
    m["containers"] = Statistics->Containers;
    m["volumes"] = Statistics->Volumes;
    m["clients"] = Statistics->Clients;
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

    return TError::Success();
}

class TNetTos : public TProperty {
public:
    TError Set(const std::string &tos) {
        return TError(EError::NotSupported, Name + " is not supported");
    }
    TError Get(std::string &value) {
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
    TError Get(std::string &value);
    TMemTotalLimit() : TProperty(D_MEM_TOTAL_LIMIT, EProperty::NONE,
                                 "Total memory limit for container "
                                 "in hierarchy") {
        IsReadOnly = true;
    }
} static MemTotalLimit;

TError TMemTotalLimit::Get(std::string &value) {
    value = std::to_string(CurrentContainer->GetHierarchyMemLimit(nullptr));

    return TError::Success();
}

void InitContainerProperties(void) {
    for (auto prop: ContainerProperties)
        prop.second->Init();
}
