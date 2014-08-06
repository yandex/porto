#include "property.hpp"

#include <map>

using namespace std;

string TContainerSpec::Get(const string &property) {
    return data[property].value;
}

bool TContainerSpec::Set(const string &property, const string &value) {
    if (data.count(property)) {
        if (data[property].checker && !data[property].checker(value))
            return false;

        data[property].value = value;
        return AppendStorage(property, value);
    }

    return false;
}

TContainerSpec::TContainerSpec(const std::string &name) : name(name) {
    storage.MountTmpfs();
}

TError TContainerSpec::SyncStorage() {
    kv::TNode node;

    for (auto &kv : data) {
        kv::TNode node;

        auto pair = node.add_pairs();
        pair->set_key(kv.first);
        pair->set_val(kv.second.value);
    }
    return storage.SaveNode(name, node);
    return TError();
}

TError TContainerSpec::AppendStorage(const string& key, const string& value) {
    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return storage.AppendNode(name, node);
}
