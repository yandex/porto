#pragma once

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "common.hpp"
#include "kvalue.hpp"
#include "container.hpp"
#include "client.hpp"

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
constexpr const char *P_CAPABILITIES_AMBIENT = "capabilities_ambient";
constexpr uint64_t CAPABILITIES_AMBIENT_SET = (1lu << 57);
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
constexpr const char *D_IO_READ = "io_read";
constexpr const char *D_IO_WRITE = "io_write";
constexpr const char *D_IO_OPS = "io_ops";
constexpr const char *D_TIME = "time";
constexpr const char *D_PORTO_STAT = "porto_stat";
constexpr const char *D_MEM_TOTAL_LIMIT = "memory_limit_total";

class TProperty {
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
    TProperty(std::string name, uint64_t set_mask, std::string desc) :
              Name(name), SetMask(set_mask), Desc(desc) {

        /* Default property is user-writable and serializable */
        IsSupported = true;
        IsReadOnly = false;
        IsHidden = false;
        IsSerializable = true;
    }
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

void InitContainerProperties(void);
