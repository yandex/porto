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

class TBindMap;
class TNetCfg;

// Property is not shown in the property list
const unsigned int SUPERUSER_PROPERTY = (1 << 0);
// Property should return parent value as default
const unsigned int PARENT_DEF_PROPERTY = (1 << 1);
// When child container is shared with parent these properties can't be changed
const unsigned int PARENT_RO_PROPERTY = (1 << 2);

extern TValueSpec propertySpec;

class TPropertyHolder {
    NO_COPY_CONSTRUCT(TPropertyHolder);
    TKeyValueStorage Storage;
    std::weak_ptr<TContainer> Container;
    const std::string Name;
    TValueHolder Holder;

    bool IsRoot();
    TError SyncStorage();
    TError AppendStorage(const std::string& key, const std::string& value);
    TError GetSharedContainer(std::shared_ptr<TContainer> &c);

public:
    TPropertyHolder(std::shared_ptr<TContainer> c) : Container(c), Name(c->GetName()), Holder(&propertySpec, c) {}
    ~TPropertyHolder();

    bool IsDefault(const std::string &property);
    std::string Get(const std::string &property);
    bool GetBool(const std::string &property);
    int GetInt(const std::string &property);
    uint64_t GetUint(const std::string &property);
    TError GetRaw(const std::string &property, std::string &value);
    TError GetRawBool(const std::string &property, bool &value);

    bool ParentDefault(std::shared_ptr<TContainer> &c,
                       const std::string &property);
    std::string GetDefault(const std::string &property);

    void SetRaw(const std::string &property, const std::string &value);
    TError Set(const std::string &property, const std::string &value);

    bool HasFlags(const std::string &property, int flags);
    bool HasState(const std::string &property, EContainerState state);

    TError Create();
    TError Restore(const kv::TNode &node);
    TError PropertyExists(const std::string &property);
};

TError RegisterProperties();

TError ParseRlimit(const std::string &s, std::map<int,struct rlimit> &rlim);
TError ParseBind(const std::string &s, std::vector<TBindMap> &dirs);
TError ParseNet(std::shared_ptr<const TContainer> container, const std::string &s, TNetCfg &net);

#endif
