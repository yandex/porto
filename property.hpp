#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "error.h"
#include "kvalue.hpp"

struct TPropertySpec {
    std::string description;
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
    TError AppendStorage(const string& key, const string& value);
    bool IsRoot();

public:
    TContainerSpec(const std::string &name);
    TContainerSpec(const std::string &name, const kv::TNode &node);
    ~TContainerSpec();
    std::string Get(const std::string &property);
    bool Set(const std::string &property, const std::string &value);
};

#endif
