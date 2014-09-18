#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "porto.hpp"
#include "kvalue.hpp"

class TContainer;

const unsigned int DYNAMIC_PROPERTY = (1 << 0);
const unsigned int HIDDEN_PROPERTY = (1 << 1);

struct TPropertySpec {
    std::string Description;
    std::string Def;
    unsigned int Flags;
    std::function<TError (std::shared_ptr<const TContainer> container, const std::string)> Valid;
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
    const std::string &Get(const std::string &property) const;
    size_t GetAsInt(const std::string &property) const;
    TError Set(std::shared_ptr<const TContainer> container, const std::string &property, const std::string &value);
    TError GetInternal(const std::string &property, std::string &value) const;
    TError SetInternal(const std::string &property, const std::string &value);
    bool IsDynamic(const std::string &property) const;
    TError Create();
    TError Restore(const kv::TNode &node);
};

#endif
