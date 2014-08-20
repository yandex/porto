#include "property.hpp"
#include "log.hpp"
#include "cgroup.hpp"
#include "util/string.hpp"

#include <map>

extern "C" {
#include <grp.h>
#include <pwd.h>
}

using namespace std;

static TError ValidUser(string user) {
    if (getpwnam(user.c_str()) == NULL)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidGroup(string group) {
    if (getgrnam(group.c_str()) == NULL)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidMemGuarantee(string str) {
    uint64_t val;

    auto memroot = TCgroup::GetRoot(TSubsystem::Memory());
    if (memroot->HasKnob("memory.low_limit_in_bytes"))
        return TError(EError::NotSupported, "invalid kernel");


    if (StringToUint64(str, val))
        return TError(EError::InvalidValue, "invalid value");

    // TODO: make sure we can really guarantee this amount of memory to
    // a container

    return TError::Success();
}

static TError ValidMemLimit(string str) {
    uint64_t val;

    if (StringToUint64(str, val))
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidCpuPolicy(string str) {
    if (str != "normal")
        return TError(EError::NotSupported, "not implemented");

    return TError::Success();
}

static TError ValidCpuPriority(string str) {
    int val;

    if (StringToInt(str, val))
        return TError(EError::InvalidValue, "invalid value");

    if (val < 0 && val > 99)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

std::map<std::string, const TPropertySpec> propertySpec = {
    {"command", { "command executed upon container start", "" }},
    {"user", { "start command with given user", "nobody", false, ValidUser }},
    {"group", { "start command with given group", "nogroup", false, ValidGroup }},
    {"env", { "container environment variables" }},
    //{"root", { "container root directory", "" }},
    {"cwd", { "container working directory", "" }},
    {"memory_guarantee", { "guaranteed amount of memory", "-1", false, ValidMemGuarantee }},
    {"memory_limit", { "memory hard limit", "-1", false, ValidMemLimit }},
    {"cpu_policy", { "CPU policy: rt, normal, idle", "normal", false, ValidCpuPolicy }},
    {"cpu_priority", { "CPU priority: 0-99", "50", false, ValidCpuPriority }},
};

string TContainerSpec::Get(const string &property) {
    if (data.find(property) == data.end())
        return propertySpec[property].def;

    return data[property];
}

bool TContainerSpec::IsRoot() {
    return name == ROOT_CONTAINER;
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

    if (propertySpec[property].Valid) {
        TError error = propertySpec[property].Valid(value);
        TLogger::LogError(error, "Can't set property");
        if (error)
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
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    for (auto &kv : data) {
        auto pair = node.add_pairs();
        pair->set_key(kv.first);
        pair->set_val(kv.second);
    }

    return storage.SaveNode(name, node);
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
