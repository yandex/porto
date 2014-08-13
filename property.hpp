#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "error.h"
#include "kvalue.hpp"

const std::string RootName = "/";

struct TPropertySpec {
    std::string description;
    std::string def;
    // can be modified in running state
    bool dynamic;
    std::function<bool (std::string)> Valid;
};

extern std::map<std::string, const TPropertySpec> propertySpec;

class TContainerSpec {
    TKeyValueStorage storage;
    std::string name;

    std::map<std::string, std::string> data;

    TError SyncStorage();
    TError AppendStorage(const std::string& key, const std::string& value);
    bool IsRoot();

public:
    TContainerSpec(const std::string &name) : name(name) { }
    ~TContainerSpec();
    std::string Get(const std::string &property);
    TError Set(const std::string &property, const std::string &value);
    bool IsDynamic(const std::string &property);
    TError Restore(const kv::TNode &node);
};

#endif
