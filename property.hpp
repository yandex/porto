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
constexpr const char *P_COMMAND = "command";
constexpr const char *P_USER = "user";
constexpr const char *P_GROUP = "group";
constexpr const char *P_ENV = "env";
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
constexpr const char *P_CPU_PRIO= "cpu_priority";
constexpr const char *P_NET_GUARANTEE = "net_guarantee";
constexpr const char *P_NET_CEIL = "net_ceil";
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
constexpr const char *P_ALLOWED_DEVICES = "allowed_devices";
constexpr const char *P_CAPABILITIES = "capabilities";
constexpr const char *P_IP = "ip";
constexpr const char *P_DEFAULT_GW = "default_gw";
constexpr const char *P_VIRT_MODE = "virt_mode";

constexpr int VIRT_MODE_APP = 1;
constexpr int VIRT_MODE_OS = 2;

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

#define SYNTHESIZE_ACCESSOR(NAME, TYPE) \
    TYPE Get ## NAME(const std::string &property) { \
        if (VariantSet.IsDefault(property)) { \
            std::shared_ptr<TContainer> c; \
            if (ParentDefault(c, property)) \
                return c->GetParent()->Prop->Get ## NAME(property); \
        } \
        return VariantSet.Get ## NAME(property); \
    } \
    TError Set ## NAME(const std::string &property, \
                       const TYPE &value) { \
        if (!VariantSet.IsValid(property)) { \
            TError error(EError::InvalidValue, property + " not found"); \
            L_ERR() << "Can't set property: " << error << std::endl; \
            return error; \
        } \
        return VariantSet.Set ## NAME(property, value); \
    } \
    TYPE GetRaw ## NAME(const std::string &property) { \
        return VariantSet.Get ## NAME(property); \
    }

class TPropertySet : public TNonCopyable {
    TVariantSet VariantSet;
    std::weak_ptr<TContainer> Container;

    TError GetSharedContainer(std::shared_ptr<TContainer> &c);

public:
    TPropertySet(std::shared_ptr<TKeyValueStorage> storage,
                std::shared_ptr<TRawValueMap> values,
                std::shared_ptr<TContainer> c,
                bool persist) :
        VariantSet(storage, values, std::to_string(c->GetId()), persist),
        Container(c) {}

    SYNTHESIZE_ACCESSOR(String, std::string);
    SYNTHESIZE_ACCESSOR(Bool, bool);
    SYNTHESIZE_ACCESSOR(Int, int);
    SYNTHESIZE_ACCESSOR(Uint, uint64_t);
    SYNTHESIZE_ACCESSOR(List, TStrList);
    SYNTHESIZE_ACCESSOR(Map, TUintMap);

    bool IsDefault(const std::string &property);
    bool ParentDefault(std::shared_ptr<TContainer> &c,
                       const std::string &property);

    bool HasFlags(const std::string &property, int flags);
    bool HasState(const std::string &property, EContainerState state);

    TError Valid(const std::string &property);

    TError Create();
    TError Restore(const kv::TNode &node);
    void Reset(const std::string &name);

    bool HasValue(const std::string &name);
    TError Flush();
    TError Sync();

    TContainerValue *GetContainerValue(const std::string &name) { return VariantSet.GetContainerValue(name); }
    std::vector<std::string> List() { return VariantSet.List(); }

    TError PrepareTaskEnv(const std::string &property,
                          std::shared_ptr<TTaskEnv> taskEnv);
};

#undef SYNTHESIZE_ACCESSOR

void RegisterProperties(std::shared_ptr<TRawValueMap> m,
                        std::shared_ptr<TContainer> c);
