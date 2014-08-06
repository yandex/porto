#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

#include "error.h"
#include "kvalue.hpp"

struct TProperty {
    bool dynamic; // can be modified in running state
    std::string value;

    std::function<bool (std::string)> checker;

    TProperty() : dynamic(false) {}
    TProperty(bool dynamic) : dynamic(dynamic) {}

    TError Sync();
};

class TContainerSpec {
    TKeyValueStorage storage;
    std::string name;

    std::map<std::string, TProperty> data = {
        {"command", TProperty(false)},
    };

    TError SyncStorage();
    TError AppendStorage(const string& key, const string& value);

public:
    TContainerSpec(const std::string &name);
    std::string Get(const std::string &property);
    bool Set(const std::string &property, const std::string &value);
};

#endif
