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

const std::string P_RAW_ROOT_PID = "root_pid";
const std::string P_RAW_UID = "uid";
const std::string P_RAW_GID = "gid";
const std::string P_RAW_ID = "id";

class TBindMap;
class TNetCfg;

// Property is not shown in the property list
const unsigned int SUPERUSER_PROPERTY = (1 << 0);
// Property should return parent value as default
const unsigned int PARENT_DEF_PROPERTY = (1 << 1);
// When child container is shared with parent these properties can't be changed
const unsigned int PARENT_RO_PROPERTY = (1 << 2);

extern TValueSet propertySet;

namespace std {
    const string &to_string(const string &s);
}

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
        error = AppendStorage(property, std::to_string(value)); \
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

    bool IsDefault(const std::string &property);
    bool ParentDefault(std::shared_ptr<TContainer> &c,
                       const std::string &property);
    std::string GetDefault(const std::string &property);

    bool HasFlags(const std::string &property, int flags);
    bool HasState(const std::string &property, EContainerState state);

    TError Create();
    TError Restore(const kv::TNode &node);
    TError PropertyExists(const std::string &property);
};

#undef SYNTHESIZE_ACCESSOR

TError RegisterProperties();

TError ParseRlimit(const std::string &s, std::map<int,struct rlimit> &rlim);
TError ParseBind(const std::string &s, std::vector<TBindMap> &dirs);
TError ParseNet(std::shared_ptr<const TContainer> container, const std::string &s, TNetCfg &net);

#endif
