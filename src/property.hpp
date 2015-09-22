#pragma once

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "common.hpp"
#include "kvalue.hpp"
#include "value.hpp"
#include "container.hpp"

constexpr const char *P_RAW_ROOT_PID = "_root_pid";
constexpr const char *P_RAW_ID = "_id";
constexpr const char *P_RAW_LOOP_DEV = "_loop_dev";
constexpr const char *P_RAW_NAME = "_name";
constexpr const char *P_RAW_START_TIME = "_start_time";
constexpr const char *P_RAW_DEATH_TIME = "_death_time";
constexpr const char *P_COMMAND = "command";
constexpr const char *P_USER = "user";
constexpr const char *P_GROUP = "group";
constexpr const char *P_ENV = "env";
constexpr const char *P_PORTO_NAMESPACE = "porto_namespace";
constexpr const char *P_ROOT = "root";
constexpr const char *P_ROOT_RDONLY = "root_readonly";
constexpr const char *P_CWD = "cwd";
constexpr const char *P_STDIN_PATH = "stdin_path";
constexpr const char *P_STDOUT_PATH = "stdout_path";
constexpr const char *P_STDERR_PATH = "stderr_path";
constexpr const char *P_STDOUT_LIMIT = "stdout_limit";
constexpr const char *P_MEM_GUARANTEE = "memory_guarantee";
constexpr const char *P_MEM_LIMIT = "memory_limit";
constexpr const char *P_RECHARGE_ON_PGFAULT = "recharge_on_pgfault";
constexpr const char *P_CPU_POLICY = "cpu_policy";
constexpr const char *P_CPU_GUARANTEE = "cpu_guarantee";
constexpr const char *P_CPU_LIMIT = "cpu_limit";
constexpr const char *P_IO_POLICY = "io_policy";
constexpr const char *P_IO_LIMIT = "io_limit";
constexpr const char *P_NET_GUARANTEE = "net_guarantee";
constexpr const char *P_NET_LIMIT = "net_limit";
constexpr const char *P_NET_PRIO = "net_priority";
constexpr const char *P_RESPAWN = "respawn";
constexpr const char *P_MAX_RESPAWNS = "max_respawns";
constexpr const char *P_ISOLATE = "isolate";
constexpr const char *P_PRIVATE = "private";
constexpr const char *P_ULIMIT = "ulimit";
constexpr const char *P_HOSTNAME = "hostname";
constexpr const char *P_BIND_DNS = "bind_dns";
constexpr const char *P_BIND = "bind";
constexpr const char *P_NET = "net";
constexpr const char *P_NET_TOS = "net_tos";
constexpr const char *P_ALLOWED_DEVICES = "allowed_devices";
constexpr const char *P_CAPABILITIES = "capabilities";
constexpr const char *P_IP = "ip";
constexpr const char *P_DEFAULT_GW = "default_gw";
constexpr const char *P_VIRT_MODE = "virt_mode";
constexpr const char *P_AGING_TIME = "aging_time";
constexpr const char *P_ENABLE_PORTO = "enable_porto";

constexpr int VIRT_MODE_APP = 0;
constexpr int VIRT_MODE_OS = 1;

class TBindMap;
class TNetCfg;
class TTaskEnv;

// Property can be modified only by privileged user
const unsigned int SUPERUSER_PROPERTY = (1 << 0);
// Property should return parent value as default
const unsigned int PARENT_DEF_PROPERTY = (1 << 1);
// When child container is shared with parent these properties can't be changed
const unsigned int PARENT_RO_PROPERTY = (1 << 2);
// Property can be modified only by restricted root
const unsigned int RESTROOT_PROPERTY = (1 << 3);
// Properties marked with this flag are reverted to default upon container
// start with virt_mode==os
const unsigned int OS_MODE_PROPERTY = (1 << 4);
// Property keeps absolute path in filesystem
const unsigned int PATH_PROPERTY = (1 << 5);

class TPropertyMap : public TValueMap {
    std::weak_ptr<TContainer> Container;

    TError GetSharedContainer(std::shared_ptr<TContainer> &c) const;

public:
    TPropertyMap(std::shared_ptr<TKeyValueNode> kvnode,
                 std::shared_ptr<TContainer> c) :
        TValueMap(kvnode),
        Container(c) {}

    std::string ToString(const std::string &name) const;
    bool ParentDefault(std::shared_ptr<TContainer> &c,
                       const std::string &property) const;

    bool HasFlags(const std::string &property, int flags) const;
    bool HasState(const std::string &property, EContainerState state) const;
    bool IsImplemented(const std::string &property) const;

    TError Check(const std::string &property) const;

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
        if (!IsValid(name))
            return TError(EError::InvalidValue, name + " not found");
        return TValueMap::Set<T>(name, value);
    }

    template<typename T>
    const T GetRaw(const std::string &name) const {
        return TValueMap::Get<T>(name);
    }
};

void RegisterProperties(std::shared_ptr<TRawValueMap> m,
                        std::shared_ptr<TContainer> c);
