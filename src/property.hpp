#pragma once

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "common.hpp"
#include "kvalue.hpp"
#include "value.hpp"
#include "container.hpp"
#include "client.hpp"

extern "C" {
#include <linux/capability.h>
}

constexpr const char *P_RAW_ROOT_PID = "_root_pid";
constexpr uint64_t ROOT_PID_SET = (1lu << 0);
constexpr const char *P_RAW_ID = "_id";
constexpr const char *P_RAW_LOOP_DEV = "_loop_dev";
constexpr uint64_t LOOP_DEV_SET = (1lu << 2);
constexpr const char *P_RAW_NAME = "_name";
constexpr const char *P_RAW_START_TIME = "_start_time";
constexpr uint64_t START_TIME_SET = (1lu << 4);
constexpr const char *P_RAW_DEATH_TIME = "_death_time";
constexpr uint64_t DEATH_TIME_SET = (1lu << 5);
constexpr const char *P_COMMAND = "command";
constexpr uint64_t COMMAND_SET = (1 << 6);
constexpr const char *P_USER = "user";
constexpr uint64_t USER_SET = (1lu << 7);
constexpr const char *P_GROUP = "group";
constexpr uint64_t GROUP_SET = (1lu << 8);
constexpr const char *P_ENV = "env";
constexpr uint64_t ENV_SET = (1 << 9);
constexpr const char *P_PORTO_NAMESPACE = "porto_namespace";
constexpr uint64_t PORTO_NAMESPACE_SET = (1lu << 10);
constexpr const char *P_ROOT = "root";
constexpr uint64_t ROOT_SET = (1 << 11);
constexpr const char *P_ROOT_RDONLY = "root_readonly";
constexpr uint64_t ROOT_RDONLY_SET = (1 << 12);
constexpr const char *P_CWD = "cwd";
constexpr uint64_t CWD_SET = (1 << 13);
constexpr const char *P_STDIN_PATH = "stdin_path";
constexpr uint64_t STDIN_SET = (1 << 14);
constexpr const char *P_STDOUT_PATH = "stdout_path";
constexpr uint64_t STDOUT_SET = (1 << 15);
constexpr const char *P_STDERR_PATH = "stderr_path";
constexpr uint64_t STDERR_SET = (1 << 16);
constexpr const char *P_STDOUT_LIMIT = "stdout_limit";
constexpr uint64_t STDOUT_LIMIT_SET = (1lu << 17);
constexpr const char *P_MEM_GUARANTEE = "memory_guarantee";
constexpr uint64_t MEM_GUARANTEE_SET = (1lu << 20);
constexpr const char *P_MEM_LIMIT = "memory_limit";
constexpr uint64_t MEM_LIMIT_SET = (1lu << 21);
constexpr const char *P_DIRTY_LIMIT = "dirty_limit";
constexpr uint64_t DIRTY_LIMIT_SET = (1lu << 22);
constexpr const char *P_ANON_LIMIT = "anon_limit";
constexpr uint64_t ANON_LIMIT_SET = (1lu << 23);
constexpr const char *P_RECHARGE_ON_PGFAULT = "recharge_on_pgfault";
constexpr uint64_t RECHARGE_ON_PGFAULT_SET = (1lu << 24);
constexpr const char *P_CPU_POLICY = "cpu_policy";
constexpr uint64_t CPU_POLICY_SET = (1lu << 25);
constexpr const char *P_CPU_GUARANTEE = "cpu_guarantee";
constexpr uint64_t CPU_GUARANTEE_SET = (1lu << 26);
constexpr const char *P_CPU_LIMIT = "cpu_limit";
constexpr uint64_t CPU_LIMIT_SET = (1lu << 27);
constexpr const char *P_IO_POLICY = "io_policy";
constexpr uint64_t IO_POLICY_SET = (1lu << 28);
constexpr const char *P_IO_LIMIT = "io_limit";
constexpr uint64_t IO_LIMIT_SET = (1lu << 29);
constexpr const char *P_IO_OPS_LIMIT = "io_ops_limit";
constexpr uint64_t IO_OPS_LIMIT_SET = (1lu << 30);
constexpr const char *P_NET_GUARANTEE = "net_guarantee";
constexpr uint64_t NET_GUARANTEE_SET = (1lu << 31);
constexpr const char *P_NET_LIMIT = "net_limit";
constexpr uint64_t NET_LIMIT_SET = (1lu << 32);
constexpr const char *P_NET_PRIO = "net_priority";
constexpr uint64_t NET_PRIO_SET = (1lu << 33);
constexpr const char *P_RESPAWN = "respawn";
constexpr uint64_t RESPAWN_SET = (1lu << 34);
constexpr const char *P_MAX_RESPAWNS = "max_respawns";
constexpr uint64_t MAX_RESPAWNS_SET = (1lu << 35);
constexpr const char *P_ISOLATE = "isolate";
constexpr uint64_t ISOLATE_SET = (1lu << 36);
constexpr const char *P_PRIVATE = "private";
constexpr uint64_t PRIVATE_SET = (1lu << 37);
constexpr const char *P_ULIMIT = "ulimit";
constexpr uint64_t ULIMIT_SET = (1lu << 38);
constexpr const char *P_HOSTNAME = "hostname";
constexpr uint64_t HOSTNAME_SET = (1lu << 39);
constexpr const char *P_BIND_DNS = "bind_dns";
constexpr uint64_t BIND_DNS_SET = (1lu << 40);
constexpr const char *P_BIND = "bind";
constexpr uint64_t BIND_SET = (1lu << 41);
constexpr const char *P_NET = "net";
constexpr uint64_t NET_SET = (1lu << 42);
constexpr const char *P_NET_TOS = "net_tos";
constexpr uint64_t NET_TOS_SET = (1lu << 43);
constexpr const char *P_DEVICES = "devices";
constexpr uint64_t DEVICES_SET = (1lu << 44);
constexpr const char *P_CAPABILITIES = "capabilities";
constexpr uint64_t CAPABILITIES_SET = (1lu << 45);
constexpr const char *P_IP = "ip";
constexpr uint64_t IP_SET = (1lu << 46);
constexpr const char *P_DEFAULT_GW = "default_gw";
constexpr uint64_t DEFAULT_GW_SET = (1lu << 47);
constexpr const char *P_VIRT_MODE = "virt_mode";
constexpr uint64_t VIRT_MODE_SET = (1lu << 48);
constexpr const char *P_AGING_TIME = "aging_time";
constexpr uint64_t AGING_TIME_SET = (1lu << 49);
constexpr const char *P_ENABLE_PORTO = "enable_porto";
constexpr uint64_t ENABLE_PORTO_SET = (1lu << 50);
constexpr const char *P_RESOLV_CONF = "resolv_conf";
constexpr uint64_t RESOLV_CONF_SET = (1lu << 51);
constexpr const char *P_WEAK = "weak";
constexpr uint64_t WEAK_SET = (1lu << 52);
constexpr const char *P_MEM_TOTAL_GUARANTEE = "memory_guarantee_total";

constexpr int VIRT_MODE_APP = 0;
constexpr const char *P_VIRT_MODE_APP = "app";
constexpr int VIRT_MODE_OS = 1;
constexpr const char *P_VIRT_MODE_OS = "os";
constexpr const char *P_CMD_VIRT_MODE_OS = "/sbin/init";

constexpr const char *D_ABSOLUTE_NAME = "absolute_name";
constexpr const char *D_ABSOLUTE_NAMESPACE = "absolute_namespace";
constexpr const char *D_STATE = "state";
constexpr uint64_t STATE_SET = (1lu << 53);
constexpr const char *D_OOM_KILLED = "oom_killed";
constexpr uint64_t OOM_KILLED_SET = (1lu << 54);
constexpr const char *D_PARENT = "parent";
constexpr const char *D_RESPAWN_COUNT = "respawn_count";
constexpr uint64_t RESPAWN_COUNT_SET = (1lu << 55);
constexpr const char *D_ROOT_PID = "root_pid";
constexpr const char *D_EXIT_STATUS = "exit_status";
constexpr uint64_t EXIT_STATUS_SET = (1lu << 56);
constexpr const char *D_START_ERRNO = "start_errno";
constexpr const char *D_STDOUT = "stdout";
constexpr const char *D_STDOUT_OFFSET = "stdout_offset";
constexpr const char *D_STDERR = "stderr";
constexpr const char *D_STDERR_OFFSET = "stderr_offset";
constexpr const char *D_MEMORY_USAGE = "memory_usage";
constexpr const char *D_ANON_USAGE = "anon_usage";
constexpr const char *D_MINOR_FAULTS = "minor_faults";
constexpr const char *D_MAJOR_FAULTS = "major_faults";
constexpr const char *D_MAX_RSS = "max_rss";
constexpr const char *D_CPU_USAGE = "cpu_usage";
constexpr const char *D_CPU_SYSTEM = "cpu_usage_system";
constexpr const char *D_NET_BYTES = "net_bytes";
constexpr const char *D_NET_PACKETS = "net_packets";
constexpr const char *D_NET_DROPS = "net_drops";
constexpr const char *D_NET_OVERLIMITS = "net_overlimits";
constexpr const char *D_NET_RX_BYTES = "net_rx_bytes";
constexpr const char *D_NET_RX_PACKETS = "net_rx_packets";
constexpr const char *D_NET_RX_DROPS = "net_rx_drops";

class TBindMap;
class TTaskEnv;

class TPropertyMap : public TValueMap {
    std::weak_ptr<TContainer> Container;

    TError GetSharedContainer(std::shared_ptr<TContainer> &c) const;

public:
    TPropertyMap(std::shared_ptr<TKeyValueNode> kvnode,
                 std::shared_ptr<TContainer> c) :
        TValueMap(kvnode),
        Container(c) {}

    bool ParentDefault(std::shared_ptr<TContainer> &c,
                       const std::string &property) const;

    bool HasFlags(const std::string &property, int flags) const;

    TError PrepareTaskEnv(const std::string &property, TTaskEnv &taskEnv);

    template<typename T>
    const T Get(const std::string &name) const {
        if (IsDefault(name)) {
            std::shared_ptr<TContainer> c;
            if (ParentDefault(c, name))
                if (c && c->GetParent())
                    return c->GetParent()->Prop->Get<T>(name);
        }

        return TValueMap::Get<T>(name);
    }

    template<typename T>
    TError Set(const std::string &name, const T& value) {
        return TValueMap::Set<T>(name, value);
    }

    template<typename T>
    const T GetRaw(const std::string &name) const {
        return TValueMap::Get<T>(name);
    }
};

void RegisterProperties(std::shared_ptr<TRawValueMap> m,
                        std::shared_ptr<TContainer> c);

class TContainerProperty {
public:
    std::string Name;
    uint64_t SetMask;
    std::string Desc;
    bool IsSupported;
    bool IsReadOnly;
    bool IsHidden;
    bool IsSerializable;
    TError IsAliveAndStopped(void);
    TError IsAlive(void);
    TError IsDead(void);
    TError IsRunning(void);
    virtual TError Set(const std::string &value) {
        if (IsReadOnly)
            return TError(EError::InvalidValue, "Read-only value: " + Name);

        return TError::Success();
    }
    virtual TError Get(std::string &value) = 0;
    TContainerProperty(std::string name, uint64_t set_mask,
                       std::string desc, bool hidden = false,
                       bool serializable = true)
                       : Name(name), SetMask(set_mask), Desc(desc),
                       IsSupported(true), IsReadOnly(false), IsHidden(hidden),
                       IsSerializable(serializable) {}

    TContainerProperty(std::string name, std::string desc,
                       bool hidden = false, bool serializable = false)
                       : Name(name), SetMask(0), Desc(desc), IsSupported(true),
                       IsReadOnly(true), IsHidden(hidden),
                       IsSerializable(serializable) {}

    virtual TError GetIndexed(const std::string &index, std::string &value) {
        return TError(EError::InvalidValue, "Invalid subscript for property");
    }
    virtual TError SetIndexed(const std::string &index, const std::string &value) {
        return TError(EError::InvalidValue, "Invalid subscript for property");
    }

    virtual TError GetToSave(std::string &value) {
        if (IsSerializable)
            return Get(value);
        else
            return TError(EError::Unknown, "Trying to save non-serializable value");
    }
    virtual TError SetFromRestore(const std::string &value) {
        if (IsSerializable)
            return Set(value);
        else
            return TError(EError::Unknown, "Trying to restore non-serializable value");
    }
};

class TContainerUser : public TContainerProperty {
public:
    TError Set(const std::string &username);
    TError Get(std::string &value);
    TContainerUser(std::string name, uint64_t set_mask, std::string desc)
                   : TContainerProperty(name, set_mask, desc) {}
};


class TContainerGroup : public TContainerProperty {
public:
    TError Set(const std::string &groupname);
    TError Get(std::string &value);
    TContainerGroup(std::string name, uint64_t set_mask, std::string desc)
                    : TContainerProperty(name, set_mask, desc) {}
};

class TContainerMemoryGuarantee : public TContainerProperty {
public:
    TError Set(const std::string &mem_guarantee);
    TError Get(std::string &value);
    TContainerMemoryGuarantee(std::string name, uint64_t set_mask,
                              std::string desc)
                              : TContainerProperty(name, set_mask, desc) {}
    TError Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();

        return TError::Success();
    }
};

class TContainerMemTotalGuarantee : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerMemTotalGuarantee(std::string name, std::string desc)
                                : TContainerProperty(name, desc) {}
    TError Init(void) {
        IsSupported = MemorySubsystem.SupportGuarantee();

        return TError::Success();
    }
};

class TContainerVirtMode : public TContainerProperty {
public:
    TError Set(const std::string &virt_mode);
    TError Get(std::string &value);
    TContainerVirtMode(std::string name, uint64_t set_mask, std::string desc)
                       : TContainerProperty(name, set_mask, desc) {}
};

class TContainerCommand : public TContainerProperty {
public:
    TError Set(const std::string &command);
    TError Get(std::string &value);
    TContainerCommand(std::string name, uint64_t set_mask, std::string desc)
                      : TContainerProperty(name, set_mask, desc) {}
};

class TContainerCwd : public TContainerProperty {
public:
    TError Set(const std::string &cwd);
    TError Get(std::string &value);
    TContainerCwd(std::string name, uint64_t set_mask, std::string desc)
                  : TContainerProperty(name, set_mask, desc) {}
    void Propagate(const std::string &value);
};

class TContainerStdinPath : public TContainerProperty {
public:
    TError Set(const std::string &path);
    TError Get(std::string &value);
    TContainerStdinPath(std::string name, uint64_t set_mask, std::string desc)
                    : TContainerProperty(name, set_mask, desc) {}
};

class TContainerStdoutPath : public TContainerProperty {
public:
    TError Set(const std::string &path);
    TError Get(std::string &value);
    TContainerStdoutPath(std::string name, uint64_t set_mask, std::string desc)
                     : TContainerProperty(name, set_mask, desc) {}
};

class TContainerStderrPath : public TContainerProperty {
public:
    TError Set(const std::string &path);
    TError Get(std::string &value);
    TContainerStderrPath(std::string name, uint64_t set_mask, std::string desc)
                     : TContainerProperty(name, set_mask, desc) {}
};

class TContainerBindDns : public TContainerProperty {
public:
    TError Set(const std::string &bind_needed);
    TError Get(std::string &value);
    TContainerBindDns(std::string name, uint64_t set_mask, std::string desc)
                    : TContainerProperty(name, set_mask, desc) {}
};

class TContainerIsolate : public TContainerProperty {
public:
    TError Set(const std::string &isolate_needed);
    TError Get(std::string &value);
    TContainerIsolate(std::string name, uint64_t set_mask, std::string desc)
                      : TContainerProperty(name, set_mask, desc) {}
};

class TContainerRoot : public TContainerProperty {
public:
    TError Set(const std::string &root);
    TError Get(std::string &value);
    TContainerRoot(std::string name, uint64_t set_mask, std::string desc)
                   : TContainerProperty(name, set_mask, desc) {}
};

class TContainerNet : public TContainerProperty {
public:
    TError Set(const std::string &net_desc);
    TError Get(std::string &value);
    TContainerNet(std::string name, uint64_t set_mask, std::string desc)
                  : TContainerProperty(name, set_mask, desc) {}
};

class TContainerHostname : public TContainerProperty {
public:
    TError Set(const std::string &hostname);
    TError Get(std::string &value);
    TContainerHostname(std::string name, uint64_t set_mask, std::string desc)
                       : TContainerProperty(name, set_mask, desc) {}
};

class TContainerRootRo : public TContainerProperty {
public:
    TError Set(const std::string &ro);
    TError Get(std::string &value);
    TContainerRootRo(std::string name, uint64_t set_mask, std::string desc)
                     : TContainerProperty(name, set_mask, desc) {}
};

class TContainerEnv : public TContainerProperty {
public:
    TError Set(const std::string &env);
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TError SetIndexed(const std::string &index, const std::string &env_val);
    TContainerEnv(std::string name, uint64_t set_mask, std::string desc)
                  : TContainerProperty(name, set_mask, desc) {}
};

class TContainerBind : public TContainerProperty {
public:
    TError Set(const std::string &bind_str);
    TError Get(std::string &value);
    TContainerBind(std::string name, uint64_t set_mask, std::string desc)
                   : TContainerProperty(name, set_mask, desc) {}
};

class TContainerIp : public TContainerProperty {
public:
    TError Set(const std::string &ipaddr);
    TError Get(std::string &value);
    TContainerIp(std::string name, uint64_t set_mask, std::string desc)
                 : TContainerProperty(name, set_mask, desc) {}
};

#ifndef CAP_AUDIT_READ
#define CAP_AUDIT_READ 37
#define CAP_BLOCK_SUSPEND 36
#endif

#define CAP_MASK(CAP) (1ULL << CAP)
constexpr const uint64_t PermittedCaps = CAP_MASK(CAP_CHOWN) |
                                         CAP_MASK(CAP_DAC_OVERRIDE) |
                                         CAP_MASK(CAP_FOWNER) |
                                         CAP_MASK(CAP_FSETID) |
                                         CAP_MASK(CAP_KILL) |
                                         CAP_MASK(CAP_SETGID) |
                                         CAP_MASK(CAP_SETUID) |
                                         CAP_MASK(CAP_NET_BIND_SERVICE) |
                                         CAP_MASK(CAP_NET_ADMIN) |
                                         CAP_MASK(CAP_NET_RAW) |
                                         CAP_MASK(CAP_IPC_LOCK) |
                                         CAP_MASK(CAP_SYS_CHROOT) |
                                         CAP_MASK(CAP_SYS_RESOURCE) |
                                         CAP_MASK(CAP_MKNOD) |
                                         CAP_MASK(CAP_AUDIT_WRITE);

class TContainerCapabilities : public TContainerProperty {
public:
    const std::map<std::string, uint64_t> SupportedCaps = {
        { "AUDIT_READ",         CAP_AUDIT_READ },
        { "CHOWN",              CAP_CHOWN },
        { "DAC_OVERRIDE",       CAP_DAC_OVERRIDE },
        { "DAC_READ_SEARCH",    CAP_DAC_READ_SEARCH },
        { "FOWNER",             CAP_FOWNER },
        { "FSETID",             CAP_FSETID },
        { "KILL",               CAP_KILL },
        { "SETGID",             CAP_SETGID },
        { "SETUID",             CAP_SETUID },
        { "SETPCAP",            CAP_SETPCAP },
        { "LINUX_IMMUTABLE",    CAP_LINUX_IMMUTABLE },
        { "NET_BIND_SERVICE",   CAP_NET_BIND_SERVICE },
        { "NET_BROADCAST",      CAP_NET_BROADCAST },
        { "NET_ADMIN",          CAP_NET_ADMIN },
        { "NET_RAW",            CAP_NET_RAW },
        { "IPC_LOCK",           CAP_IPC_LOCK },
        { "IPC_OWNER",          CAP_IPC_OWNER },
        { "SYS_MODULE",         CAP_SYS_MODULE },
        { "SYS_RAWIO",          CAP_SYS_RAWIO },
        { "SYS_CHROOT",         CAP_SYS_CHROOT },
        { "SYS_PTRACE",         CAP_SYS_PTRACE },
        { "SYS_PACCT",          CAP_SYS_PACCT },
        { "SYS_ADMIN",          CAP_SYS_ADMIN },
        { "SYS_BOOT",           CAP_SYS_BOOT },
        { "SYS_NICE",           CAP_SYS_NICE },
        { "SYS_RESOURCE",       CAP_SYS_RESOURCE },
        { "SYS_TIME",           CAP_SYS_TIME },
        { "SYS_TTY_CONFIG",     CAP_SYS_TTY_CONFIG },
        { "MKNOD",              CAP_MKNOD },
        { "LEASE",              CAP_LEASE },
        { "AUDIT_WRITE",        CAP_AUDIT_WRITE },
        { "AUDIT_CONTROL",      CAP_AUDIT_CONTROL },
        { "SETFCAP",            CAP_SETFCAP },
        { "MAC_OVERRIDE",       CAP_MAC_OVERRIDE },
        { "MAC_ADMIN",          CAP_MAC_ADMIN },
        { "SYSLOG",             CAP_SYSLOG },
        { "WAKE_ALARM",         CAP_WAKE_ALARM },
        { "BLOCK_SUSPEND",      CAP_BLOCK_SUSPEND },
    };
    uint64_t AllCaps;
    TError Init(void) {
        AllCaps = 0xffffffffffffffff >> (63 - LastCapability);

        return TError::Success();
    }
    TError Set(const std::string &caps);
    TError Get(std::string &value);
    TContainerCapabilities(std::string name, uint64_t set_mask, std::string desc)
                           : TContainerProperty(name, set_mask, desc, true) {}
};

class TContainerDefaultGw : public TContainerProperty {
public:
    TError Set(const std::string &gw);
    TError Get(std::string &value);
    TContainerDefaultGw(std::string name, uint64_t set_mask, std::string desc)
                        : TContainerProperty(name, set_mask, desc, true) {}
};

class TContainerResolvConf : public TContainerProperty {
public:
    TError Set(const std::string &conf);
    TError Get(std::string &value);
    TContainerResolvConf(std::string name, uint64_t set_mask, std::string desc)
                         : TContainerProperty(name, set_mask, desc) {}
};

class TContainerDevices : public TContainerProperty {
public:
    TError Set(const std::string &dev);
    TError Get(std::string &value);
    TContainerDevices(std::string name, uint64_t set_mask, std::string desc)
                      : TContainerProperty(name, set_mask, desc) {}
};

class TContainerRawRootPid : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TContainerRawRootPid(std::string name, std::string desc)
                         : TContainerProperty(name, desc, true, true) {
        SetMask = ROOT_PID_SET;
    }
};

class TContainerRawLoopDev : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TContainerRawLoopDev(std::string name, std::string desc)
                    : TContainerProperty(name, desc, true, true) {
        SetMask = LOOP_DEV_SET;
    }
};

class TContainerRawStartTime : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TContainerRawStartTime(std::string name, std::string desc)
                           : TContainerProperty(name, desc, true, true) {
        SetMask = START_TIME_SET;
    }
};

class TContainerRawDeathTime : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TContainerRawDeathTime(std::string name, std::string desc)
                           : TContainerProperty(name, desc, true, true) {
        SetMask = DEATH_TIME_SET;
    }
};

class TContainerUlimit : public TContainerProperty {
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
        { "stask", RLIMIT_STACK },
    };
public:
    TError Set(const std::string &ulimit);
    TError Get(std::string &value);
    TContainerUlimit(std::string name, uint64_t set_mask, std::string desc)
                     : TContainerProperty(name, set_mask, desc) {}
    void Propagate(std::map<int, struct rlimit> &ulimits);
};

class TContainerPortoNamespace : public TContainerProperty {
public:
    TError Set(const std::string &ns);
    TError Get(std::string &value);
    TContainerPortoNamespace(std::string name, uint64_t set_mask,
                              std::string desc)
                              : TContainerProperty(name, set_mask, desc) {}
};

class TContainerStdoutLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TContainerStdoutLimit(std::string name, uint64_t set_mask,
                          std::string desc)
                          : TContainerProperty(name, set_mask, desc) {}
};

class TContainerMemoryLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TContainerMemoryLimit(std::string name, uint64_t set_mask,
                          std::string desc)
                          : TContainerProperty(name, set_mask, desc) {}
};

class TContainerAnonLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TContainerAnonLimit(std::string name, uint64_t set_mask,
                        std::string desc)
                        : TContainerProperty(name, set_mask, desc) {}
    TError Init(void) {
        IsSupported = MemorySubsystem.SupportAnonLimit();

        return TError::Success();
    }

};

class TContainerDirtyLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TContainerDirtyLimit(std::string name, uint64_t set_mask,
                        std::string desc)
                        : TContainerProperty(name, set_mask, desc) {}
    TError Init(void) {
        IsSupported = MemorySubsystem.SupportDirtyLimit();

        return TError::Success();
    }

};

class TContainerRechargeOnPgfault : public TContainerProperty {
public:
    TError Set(const std::string &recharge);
    TError Get(std::string &value);
    TContainerRechargeOnPgfault(std::string name, uint64_t set_mask,
                        std::string desc)
                        : TContainerProperty(name, set_mask, desc) {}
    TError Init(void) {
        IsSupported = MemorySubsystem.SupportRechargeOnPgfault();

        return TError::Success();
    }

};

class TContainerCpuPolicy : public TContainerProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TContainerCpuPolicy(std::string name, uint64_t set_mask,
                        std::string desc)
                        : TContainerProperty(name, set_mask, desc) {}
    TError Propagate(const std::string &policy);
};

class TContainerCpuLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TContainerCpuLimit(std::string name, uint64_t set_mask,
                       std::string desc)
                       : TContainerProperty(name, set_mask, desc) {}
};

class TContainerCpuGuarantee : public TContainerProperty {
public:
    TError Set(const std::string &guarantee);
    TError Get(std::string &value);
    TContainerCpuGuarantee(std::string name, uint64_t set_mask,
                           std::string desc)
                           : TContainerProperty(name, set_mask, desc) {}
};

class TContainerIoPolicy : public TContainerProperty {
public:
    TError Set(const std::string &policy);
    TError Get(std::string &value);
    TContainerIoPolicy(std::string name, uint64_t set_mask,
                       std::string desc)
                       : TContainerProperty(name, set_mask, desc) {}
    TError Init(void) {
        IsSupported = BlkioSubsystem.SupportPolicy();

        return TError::Success();
    }
    TError Propagate(const std::string &policy);
};

class TContainerIoLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TContainerIoLimit(std::string name, uint64_t set_mask,
                      std::string desc)
                      : TContainerProperty(name, set_mask, desc) {}
    TError Init(void) {
        IsSupported = MemorySubsystem.SupportIoLimit();

        return TError::Success();
    }
};

class TContainerIopsLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TContainerIopsLimit(std::string name, uint64_t set_mask,
                        std::string desc)
                        : TContainerProperty(name, set_mask, desc) {}
    TError Init(void) {
        IsSupported = MemorySubsystem.SupportIoLimit();

        return TError::Success();
    }
};

class TContainerNetGuarantee : public TContainerProperty {
public:
    TError Set(const std::string &guarantee);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &guarantee);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetGuarantee(std::string name, uint64_t set_mask,
                           std::string desc)
                           : TContainerProperty(name, set_mask, desc) {}
};

class TContainerNetLimit : public TContainerProperty {
public:
    TError Set(const std::string &limit);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &limit);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetLimit(std::string name, uint64_t set_mask,
                           std::string desc)
                           : TContainerProperty(name, set_mask, desc) {}
};

class TContainerNetPriority : public TContainerProperty {
public:
    TError Set(const std::string &prio);
    TError Get(std::string &value);
    TError SetIndexed(const std::string &index, const std::string &prio);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetPriority(std::string name, uint64_t set_mask,
                           std::string desc)
                           : TContainerProperty(name, set_mask, desc) {}
};

class TContainerRespawn : public TContainerProperty {
public:
    TError Set(const std::string &respawn);
    TError Get(std::string &value);
    TContainerRespawn(std::string name, uint64_t set_mask,
                      std::string desc)
                      : TContainerProperty(name, set_mask, desc) {}
};

class TContainerMaxRespawns : public TContainerProperty {
public:
    TError Set(const std::string &max);
    TError Get(std::string &value);
    TContainerMaxRespawns(std::string name, uint64_t set_mask,
                          std::string desc)
                          : TContainerProperty(name, set_mask, desc) {}
};

class TContainerPrivate : public TContainerProperty {
public:
    TError Set(const std::string &max);
    TError Get(std::string &value);
    TContainerPrivate(std::string name, uint64_t set_mask,
                      std::string desc)
                      : TContainerProperty(name, set_mask, desc) {}
};

class TContainerNetTos : public TContainerProperty {
public:
    TError Set(const std::string &tos) {
        return TError(EError::NotSupported, Name + " is not supported");
    }
    TError Get(std::string &value) {
        return TError(EError::NotSupported, "Not supported: " + Name);
    }
    TContainerNetTos(std::string name, uint64_t set_mask,
                     std::string desc)
                     : TContainerProperty(name, set_mask, desc) {}
};

class TContainerAgingTime : public TContainerProperty {
public:
    TError Set(const std::string &time);
    TError Get(std::string &value);
    TContainerAgingTime(std::string name, uint64_t set_mask, std::string desc)
                        : TContainerProperty(name, set_mask, desc) {}
};

class TContainerEnablePorto : public TContainerProperty {
public:
    TError Set(const std::string &enabled);
    TError Get(std::string &value);
    TContainerEnablePorto(std::string name, uint64_t set_mask, std::string desc)
                          : TContainerProperty(name, set_mask, desc) {}
    void Propagate(bool value);
};

class TContainerWeak : public TContainerProperty {
public:
    TError Set(const std::string &weak);
    TError Get(std::string &value);
    TContainerWeak(std::string name, uint64_t set_mask, std::string desc)
                          : TContainerProperty(name, set_mask, desc) {}
};

/* Read-only properties derived from data filelds follow below... */

class TContainerAbsoluteName : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerAbsoluteName(std::string name, std::string desc)
                           : TContainerProperty(name, desc) {}
};

class TContainerAbsoluteNamespace : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerAbsoluteNamespace(std::string name, std::string desc)
                                : TContainerProperty(name, desc) {}
};

class TContainerState : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TContainerState(std::string name, std::string desc)
                    : TContainerProperty(name, desc, false, true) {
        SetMask = STATE_SET;
    }
};

class TContainerOomKilled : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError GetToSave(std::string &value);
    TError Get(std::string &value);
    TContainerOomKilled(std::string name, std::string desc)
                    : TContainerProperty(name, desc, false, true) {
        SetMask = OOM_KILLED_SET;
    }
};

class TContainerParent : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerParent(std::string name, std::string desc)
                     : TContainerProperty(name, desc, true, false) {}
};

class TContainerRespawnCount : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError Get(std::string &value);
    TContainerRespawnCount(std::string name, std::string desc)
                           : TContainerProperty(name, desc, false, true) {
        SetMask = RESPAWN_COUNT_SET;
    }
};

class TContainerRootPid : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerRootPid(std::string name, std::string desc)
                      : TContainerProperty(name, desc, true) {}
};

class TContainerExitStatus : public TContainerProperty {
public:
    TError SetFromRestore(const std::string &value);
    TError GetToSave(std::string &value);
    TError Get(std::string &value);
    TContainerExitStatus(std::string name, std::string desc)
                         : TContainerProperty(name, desc, false, true) {
        SetMask = EXIT_STATUS_SET;
    }
};

class TContainerStartErrno : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerStartErrno(std::string name, std::string desc)
                         : TContainerProperty(name, desc) {}
};

class TContainerStdout : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerStdout(std::string name, std::string desc)
                     : TContainerProperty(name, desc) {}
};

class TContainerStdoutOffset : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerStdoutOffset(std::string name, std::string desc)
                           : TContainerProperty(name, desc) {}
};


class TContainerStderr : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerStderr(std::string name, std::string desc)
                     : TContainerProperty(name, desc) {}
};

class TContainerStderrOffset : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerStderrOffset(std::string name, std::string desc)
                           : TContainerProperty(name, desc) {}
};

class TContainerMemUsage : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerMemUsage(std::string name, std::string desc)
                       : TContainerProperty(name, desc) {}
};

class TContainerAnonUsage : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerAnonUsage(std::string name, std::string desc)
                       : TContainerProperty(name, desc) {}
};

class TContainerMinorFaults : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerMinorFaults(std::string name, std::string desc)
                          : TContainerProperty(name, desc) {}
};

class TContainerMajorFaults : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerMajorFaults(std::string name, std::string desc)
                          : TContainerProperty(name, desc) {}
};

class TContainerMaxRss : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerMaxRss(std::string name, std::string desc)
                     : TContainerProperty(name, desc) {}
    TError Init(void) {
        TCgroup rootCg = MemorySubsystem.RootCgroup();
        TUintMap stat;

        TError error = MemorySubsystem.Statistics(rootCg, stat);
        IsSupported = !error && (stat.find("total_max_rss") != stat.end());

        return TError::Success();
    }
};

class TContainerCpuUsage : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerCpuUsage(std::string name, std::string desc)
                       : TContainerProperty(name, desc) {}
};

class TContainerCpuSystem : public TContainerProperty {
public:
    TError Get(std::string &value);
    TContainerCpuSystem(std::string name, std::string desc)
                        : TContainerProperty(name, desc) {}
};

class TContainerNetBytes : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetBytes(std::string name, std::string desc)
                       : TContainerProperty(name, desc) {}
};

class TContainerNetPackets : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetPackets(std::string name, std::string desc)
                         : TContainerProperty(name, desc) {}
};

class TContainerNetDrops : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetDrops(std::string name, std::string desc)
                       : TContainerProperty(name, desc) {}
};

class TContainerNetOverlimits : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetOverlimits(std::string name, std::string desc)
                            : TContainerProperty(name, desc) {}
};

class TContainerNetRxBytes : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetRxBytes(std::string name, std::string desc)
                         : TContainerProperty(name, desc) {}
};

class TContainerNetRxPackets : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetRxPackets(std::string name, std::string desc)
                           : TContainerProperty(name, desc) {}
};

class TContainerNetRxDrops : public TContainerProperty {
public:
    TError Get(std::string &value);
    TError GetIndexed(const std::string &index, std::string &value);
    TContainerNetRxDrops(std::string name, std::string desc)
                         : TContainerProperty(name, desc) {}
};

void InitContainerProperties(void);
