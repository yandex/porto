#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "porto.hpp"
#include "kvalue.hpp"

class TContainer;

// Property can be changed while container is running
const unsigned int DYNAMIC_PROPERTY = (1 << 0);
// Property is not shown in the property list
const unsigned int HIDDEN_PROPERTY = (1 << 1);
// Property can be changed only by super user
const unsigned int SUPERUSER_PROPERTY = (1 << 2);
// Property required cgroups and/or namespaces
const unsigned int CGNSREQ_PROPERTY = (1 << 3);

struct TPropertySpec {
    std::string Description;
    std::function<std::string(std::shared_ptr<const TContainer>)> Default;
    unsigned int Flags;
    std::function<TError(std::shared_ptr<const TContainer> container, const std::string)> Valid;
};

extern std::map<std::string, const TPropertySpec> propertySpec;

class TContainerSpec {
    TKeyValueStorage Storage;
    std::string Name;
    std::map<std::string, std::string> Data;

    TError SyncStorage();
    TError AppendStorage(const std::string& key, const std::string& value);
    bool IsRoot() const;

public:
    TContainerSpec(const std::string &name) : Name(name) { }
    ~TContainerSpec();
    std::string Get(std::shared_ptr<const TContainer> container, const std::string &property) const;
    TError Set(std::shared_ptr<const TContainer> container, const std::string &property, const std::string &value);
    TError GetRaw(const std::string &property, std::string &value) const;
    TError SetRaw(const std::string &property, const std::string &value);
    unsigned int GetFlags(const std::string &property) const;
    TError Create();
    TError Restore(const kv::TNode &node);
};

#endif
