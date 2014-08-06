#include "property.hpp"

#include <map>

using namespace std;

string TContainerSpec::Get(const string &property) {
    return data[property].value;
}

void TContainerSpec::Set(const string &property, const string &value) {
    if (data[property].checker(value)) {
        data[property].value = value;
        AppendStorage(property, value);
    }
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
