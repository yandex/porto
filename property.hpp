#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "porto.hpp"
#include "kvalue.hpp"
#include "value.hpp"
#include "container.hpp"

constexpr const char *P_RAW_ROOT_PID = "root_pid";
constexpr const char *P_RAW_UID = "uid";
constexpr const char *P_RAW_GID = "gid";
constexpr const char *P_RAW_ID = "id";
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

class TBindMap;
class TNetCfg;
class TTaskEnv;

// Property is not shown in the property list
const unsigned int SUPERUSER_PROPERTY = (1 << 0);
// Property should return parent value as default
const unsigned int PARENT_DEF_PROPERTY = (1 << 1);
// When child container is shared with parent these properties can't be changed
const unsigned int PARENT_RO_PROPERTY = (1 << 2);

extern TValueSet propertySet;

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
        if (!propertySet.Valid(property)) { \
            TError error(EError::InvalidValue, "property not found"); \
            TLogger::LogError(error, "Can't set property " + property); \
            return error; \
        } \
        TError error = VariantSet.Set ## NAME(property, value); \
        if (error) \
            return error; \
        error = AppendStorage(property, NAME ## ToString(value)); \
        if (error) \
            return error; \
        return TError::Success(); \
    } \
    TYPE GetRaw ## NAME(const std::string &property) { \
        return VariantSet.Get ## NAME(property); \
    }

class TPropertySet {
    NO_COPY_CONSTRUCT(TPropertySet);
    TKeyValueStorage Storage;
    std::weak_ptr<TContainer> Container;
    const std::string Name;
    TVariantSet VariantSet;

    bool IsRoot();
    TError SyncStorage();
    TError AppendStorage(const std::string& key, const std::string& value);
    TError GetSharedContainer(std::shared_ptr<TContainer> &c);

public:
    TPropertySet(std::shared_ptr<TContainer> c) : Container(c), Name(c->GetName()), VariantSet(&propertySet, c) {}
    ~TPropertySet();

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

    TError Create();
    TError Restore(const kv::TNode &node);
    TError Valid(const std::string &property);

    TError PrepareTaskEnv(const std::string &property,
                          std::shared_ptr<TTaskEnv> taskEnv);
};

#undef SYNTHESIZE_ACCESSOR

TError RegisterProperties();

#endif
