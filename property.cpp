#include "property.hpp"

#include <map>

using namespace std;

std::map<std::string, const TPropertySpec> propertySpec = {
    {"command", { "command executed upon container start" }},
    {"low_limit", { "memory low limit in bytes" }},
    {"user", { "start command with given user" }},
    {"group", { "start command with given group" }},
};

string TContainerSpec::Get(const string &property) {
    // TODO: get default value from propertySpec if nothing in data
    return data[property];
}

bool TContainerSpec::IsRoot() {
    return name == "/";
}

bool TContainerSpec::Set(const string &property, const string &value) {
    if (propertySpec.count(property)) {
        if (propertySpec[property].Valid &&
            !propertySpec[property].Valid(value))
            return false;

        data[property] = value;
        return AppendStorage(property, value);
    }

    return false;
}

TContainerSpec::TContainerSpec(const std::string &name) : name(name) {
}

TContainerSpec::TContainerSpec(const std::string &name, const kv::TNode &node) : name(name) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        data[key] = value;
    }

    SyncStorage();
}

TContainerSpec::~TContainerSpec() {
    if (!IsRoot())
        storage.RemoveNode(name);
}

TError TContainerSpec::SyncStorage() {
    if (IsRoot())
        return TError();

    kv::TNode node;

    for (auto &kv : data) {
        kv::TNode node;

        auto pair = node.add_pairs();
        pair->set_key(kv.first);
        pair->set_val(kv.second);
    }
    return storage.SaveNode(name, node);
}

TError TContainerSpec::AppendStorage(const string& key, const string& value) {
    if (IsRoot())
        return TError();

    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return storage.AppendNode(name, node);
}
