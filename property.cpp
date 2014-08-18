#include "property.hpp"
#include "log.hpp"

#include <map>

extern "C" {
#include <grp.h>
#include <pwd.h>
}

using namespace std;

static bool ValidUser(string user) {
    return getpwnam(user.c_str()) != NULL;
}

static bool ValidGroup(string group) {
    return getgrnam(group.c_str()) != NULL;
}

std::map<std::string, const TPropertySpec> propertySpec = {
    {"command", { "command executed upon container start", "" }},
    {"low_limit", { "memory low limit in bytes", "0" }},
    {"user", { "start command with given user", "nobody", false, ValidUser }},
    {"group", { "start command with given group", "nogroup", false, ValidGroup }},
    {"env", { "container environment variables" }},
    //{"root", { "container root directory", "" }},
    {"cwd", { "container working directory", "" }},
};

string TContainerSpec::Get(const string &property) {
    if (data.find(property) == data.end())
        return propertySpec[property].def;

    return data[property];
}

bool TContainerSpec::IsRoot() {
    return name == RootName;
}

bool TContainerSpec::IsDynamic(const std::string &property) {
    if (propertySpec.find(property) == propertySpec.end())
        return false;

    return propertySpec[property].dynamic;
}

string TContainerSpec::GetInternal(const string &property) {
    if (data.find(property) == data.end())
        return "";
    return data[property];
}

TError TContainerSpec::SetInternal(const string &property, const string &value) {
    data[property] = value;
    TError error(AppendStorage(property, value));
    if (error)
        TLogger::LogError(error, "Can't append property to key-value store");
    return error;
}

TError TContainerSpec::Set(const string &property, const string &value) {
    if (propertySpec.find(property) == propertySpec.end()) {
        TError error(EError::InvalidValue, "property not found");
        TLogger::LogError(error, "Can't set property");
        return error;
    }

    if (propertySpec[property].Valid &&
        !propertySpec[property].Valid(value)) {
        TError error(EError::InvalidValue, "invalid property value");
        TLogger::LogError(error, "Can't set property");
        return error;
    }

    return SetInternal(property, value);
}

TError TContainerSpec::Restore(const kv::TNode &node) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        data[key] = value;
    }

    return SyncStorage();
}

TContainerSpec::~TContainerSpec() {
    if (!IsRoot()) {
        TError error = storage.RemoveNode(name);
        TLogger::LogError(error, "Can't remove key-value node " + name);
    }
}

TError TContainerSpec::SyncStorage() {
#if 0
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    for (auto &kv : data) {
        kv::TNode node;

        auto pair = node.add_pairs();
        pair->set_key(kv.first);
        pair->set_val(kv.second);
    }
    return storage.SaveNode(name, node);
#else
    return TError::Success();
#endif
}

TError TContainerSpec::AppendStorage(const string& key, const string& value) {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return storage.AppendNode(name, node);
}
