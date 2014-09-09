#include <map>

#include "subsystem.hpp"
#include "property.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <grp.h>
#include <pwd.h>
}

using namespace std;

static TError ValidUser(std::shared_ptr<const TContainer> container, const string user) {
    if (getpwnam(user.c_str()) == NULL)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidGroup(std::shared_ptr<const TContainer> container, const string group) {
    if (getgrnam(group.c_str()) == NULL)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidMemGuarantee(std::shared_ptr<const TContainer> container, const string str) {
    uint64_t newval;

    auto memroot = memorySubsystem->GetRootCgroup();
    if (!memroot->HasKnob("memory.low_limit_in_bytes"))
        return TError(EError::NotSupported, "invalid kernel");

    if (StringToUint64(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    if (!container->ValidHierarchicalProperty("memory_guarantee", str))
        return TError(EError::InvalidValue, "invalid hierarchical value");

    uint64_t total = container->GetRoot()->GetChildrenSum("memory_guarantee", container, newval);
    if (total + MEMORY_GUARANTEE_RESERVE > GetTotalMemory())
        return TError(EError::ResourceNotAvailable, "can't guarantee all available memory");

    return TError::Success();
}

static TError ValidMemLimit(std::shared_ptr<const TContainer> container, const string str) {
    uint64_t newval;

    if (StringToUint64(str, newval))
        return TError(EError::InvalidValue, "invalid value");

    if (!container->ValidHierarchicalProperty("memory_limit", str))
        return TError(EError::InvalidValue, "invalid hierarchical value");

    return TError::Success();
}

static TError ValidCpuPolicy(std::shared_ptr<const TContainer> container, const string str) {
    if (str != "normal" && str != "rt" && str != "idle")
        return TError(EError::InvalidValue, "invalid policy");

    if (str == "rt" || str == "idle")
        return TError(EError::NotSupported, "not implemented");

    return TError::Success();
}

static TError ValidCpuPriority(std::shared_ptr<const TContainer> container, const string str) {
    int val;

    if (StringToInt(str, val))
        return TError(EError::InvalidValue, "invalid value");

    if (val < 0 || val > 99)
        return TError(EError::InvalidValue, "invalid value");

    return TError::Success();
}

static TError ValidBool(std::shared_ptr<const TContainer> container, const string str) {
    if (str != "true" && str != "false")
        return TError(EError::InvalidValue, "invalid boolean value");

    return TError::Success();
}

std::map<std::string, const TPropertySpec> propertySpec = {
    {"command", { "command executed upon container start", "" }},
    {"user", { "start command with given user", "nobody", false, ValidUser }},
    {"group", { "start command with given group", "nogroup", false, ValidGroup }},
    {"env", { "container environment variables" }},
    //{"root", { "container root directory", "" }},
    {"cwd", { "container working directory", "" }},
    {"memory_guarantee", { "guaranteed amount of memory", "0", false, ValidMemGuarantee }},
    {"memory_limit", { "memory hard limit", "0", false, ValidMemLimit }},
    {"cpu_policy", { "CPU policy: rt, normal, idle", "normal", false, ValidCpuPolicy }},
    {"cpu_priority", { "CPU priority: 0-99", "50", false, ValidCpuPriority }},
    {"respawn", { "automatically respawn dead container", "false", false, ValidBool }},
};

const string &TContainerSpec::Get(const string &property) const {
    if (Data.find(property) == Data.end())
        return propertySpec[property].Def;

    return Data.at(property);
}

size_t TContainerSpec::GetAsInt(const string &property) const {
    uint64_t val = 0;
    (void)StringToUint64(Get(property), val);
    return val;
}

bool TContainerSpec::IsRoot() const {
    return Name == ROOT_CONTAINER;
}

bool TContainerSpec::IsDynamic(const std::string &property) const {
    if (propertySpec.find(property) == propertySpec.end())
        return false;

    return propertySpec.at(property).Dynamic;
}

TError TContainerSpec::GetInternal(const string &property, string &value) const {
    if (Data.find(property) == Data.end())
        return TError(EError::InvalidValue, "Invalid property");
    value = Data.at(property);
    return TError::Success();
}

TError TContainerSpec::SetInternal(const string &property, const string &value) {
    Data[property] = value;
    TError error(AppendStorage(property, value));
    if (error)
        TLogger::LogError(error, "Can't append property to key-value store");
    return error;
}

TError TContainerSpec::Set(std::shared_ptr<const TContainer> container, const std::string &property, const std::string &value) {
    if (propertySpec.find(property) == propertySpec.end()) {
        TError error(EError::InvalidValue, "property not found");
        TLogger::LogError(error, "Can't set property");
        return error;
    }

    if (propertySpec[property].Valid) {
        TError error = propertySpec[property].Valid(container, value);
        TLogger::LogError(error, "Can't set property");
        if (error)
            return error;
    }

    return SetInternal(property, value);
}

TError TContainerSpec::Create() {
    kv::TNode node;
    return Storage.SaveNode(Name, node);
}

TError TContainerSpec::Restore(const kv::TNode &node) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        Data[key] = value;
    }

    return SyncStorage();
}

TContainerSpec::~TContainerSpec() {
    if (!IsRoot()) {
        TError error = Storage.RemoveNode(Name);
        TLogger::LogError(error, "Can't remove key-value node " + Name);
    }
}

TError TContainerSpec::SyncStorage() {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    for (auto &kv : Data) {
        auto pair = node.add_pairs();
        pair->set_key(kv.first);
        pair->set_val(kv.second);
    }

    return Storage.SaveNode(Name, node);
}

TError TContainerSpec::AppendStorage(const string& key, const string& value) {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return Storage.AppendNode(Name, node);
}
