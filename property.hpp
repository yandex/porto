#ifndef __PROPERTY_HPP__
#define __PROPERTY_HPP__

#include <map>
#include <string>
#include <memory>
#include <functional>

struct TProperty {
    bool dynamic; // can be modified in running state
    std::string value;

    std::function<bool (std::string)> checker;

    TProperty() : dynamic(false) {}
    TProperty(bool dynamic) : dynamic(dynamic) {}
};

class TContainerSpec {
    std::map<std::string, TProperty> data = {
        {"command", TProperty(false)},
    };

public:
    std::string Get(std::string property);
    void Set(std::string property, std::string value);
};

#endif
