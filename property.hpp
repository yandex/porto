#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "error.h"
#include "kvalue.hpp"

struct TProperty {
    std::string value;
    bool dynamic; // can be modified in running state

    std::function<bool (std::string)> checker;

    TProperty(std::string value = "", bool dynamic = false) : value(value),
                                                              dynamic(dynamic) {}

    TError Sync();
};

class TContainerSpec {
    TKeyValueStorage storage;
    std::string name;

    std::map<std::string, TProperty> data = {
        {"command", TProperty()},
        {"low_limit", TProperty()},
    };

    TError SyncStorage();
    TError AppendStorage(const string& key, const string& value);

public:
    TContainerSpec(const std::string &name);
    TContainerSpec(const std::string &name, const kv::TNode &node);
    std::string Get(const std::string &property);
    bool Set(const std::string &property, const std::string &value);
};

#endif
