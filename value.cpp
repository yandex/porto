#include <sstream>

#include "value.hpp"
#include "config.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

std::string TStringValue::ToString(const std::string &value) const {
    return value;
}

TError TStringValue::FromString(const std::string &value) {
    TError error = CheckValue(value);
    if (error)
        return error;

    return Set(value);
}

std::string TStringValue::GetDefault() const {
    return "";
}

std::string TIntValue::ToString(const int &value) const {
    return std::to_string(value);
}

TError TIntValue::FromString(const std::string &value) {
    int tmp;
    TError error = StringToInt(value, tmp);
    if (error)
        return TError(EError::InvalidValue, "Invalid integer value " + value);

    error = CheckValue(tmp);
    if (error)
        return error;

    return Set(tmp);
}

int TIntValue::GetDefault() const {
    return 0;
}

std::string TUintValue::ToString(const uint64_t &value) const {
    return std::to_string(value);
}

TError TUintValue::FromString(const std::string &value) {
    TError error;
    uint64_t tmp;
    if (GetFlags() & UINT_UNIT_VALUE)
        error = StringWithUnitToUint64(value, tmp);
    else
        error = StringToUint64(value, tmp);
    if (error)
        return TError(EError::InvalidValue, "Invalid unsigned integer value " + value);

    error = CheckValue(tmp);
    if (error)
        return error;

    return Set(tmp);
}

uint64_t TUintValue::GetDefault() const {
    return 0;
}

std::string TBoolValue::ToString(const bool &value) const {
    if (value)
        return "true";
    return "false";
}

TError TBoolValue::FromString(const std::string &value) {
    bool tmp;

    if (value == "true")
        tmp = true;
    else if (value == "false")
        tmp = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");

    TError error = CheckValue(tmp);
    if (error)
        return error;

    return Set(tmp);
}

bool TBoolValue::GetDefault() const {
    return false;
}

std::string TListValue::ToString(const TStrList &value) const {
    std::stringstream str;

    for (auto v : value) {
        if (str.str().length())
            str << "; ";
        str << v;
    }

    return str.str();
}

TError TListValue::FromString(const std::string &value) {
    std::vector<std::string> vec;
    TStrList m;
    TError error = SplitEscapedString(value, ';', vec);
    if (error)
        return error;

    for (auto &val : vec) {
        std::string tmp = StringTrim(val);
        if (!tmp.length())
            continue;
        m.push_back(tmp);
    }

    error = CheckValue(m);
    if (error)
        return error;

    Set(m);

    return TError::Success();
}

TStrList TListValue::GetDefault() const {
    return TStrList{};
}

std::string TMapValue::ToString(const TUintMap &value) const {
    std::stringstream str;

    for (auto kv : value) {
        if (str.str().length())
            str << "; ";
        str << kv.first << ": " << kv.second;
    }

    return str.str();
}

TError TMapValue::FromString(const std::string &value) {
    TUintMap m;
    std::vector<std::string> lines;
    TError error = SplitEscapedString(value, ';', lines);
    if (error)
        return error;

    for (auto &line : lines) {
        std::vector<std::string> nameval;

        (void)SplitEscapedString(line, ':', nameval);
        if (nameval.size() != 2)
            return TError(EError::InvalidValue, "Invalid format");

        std::string key = StringTrim(nameval[0]);
        uint64_t val;

        error = StringToUint64(nameval[1], val);
        if (error)
            return TError(EError::InvalidValue, "Invalid value " + nameval[1]);

        m[key] = val;
    }

    error = CheckValue(m);
    if (error)
        return error;

    Set(m);

    return TError::Success();
}

TUintMap TMapValue::GetDefault() const {
    return TUintMap{};
}

TRawValueMap::~TRawValueMap() {
    for (auto pair : AbstractValues)
        delete pair.second;
}

TError TRawValueMap::Add(const std::string &name, TAbstractValue *av) {
    AbstractValues[name] = av;
    return TError::Success();
}

TAbstractValue *TRawValueMap::operator[](const std::string &name) const {
    return AbstractValues.at(name);
}

TError TRawValueMap::IsValid(const std::string &name) const {
    if (AbstractValues.find(name) != AbstractValues.end())
        return TError::Success();

    return TError(EError::InvalidValue, "Invalid value " + name);
}

bool TRawValueMap::IsDefault(const std::string &name) const {
    return !HasValue(name);
}

bool TRawValueMap::HasValue(const std::string &name) const {
    TAbstractValue *p = AbstractValues.at(name);
    return p->HasValue();
}

std::vector<std::string> TRawValueMap::List() const {
    std::vector<std::string> v;
    for (auto pair : AbstractValues)
        v.push_back(pair.first);
    return v;
}

TVariantSet::~TVariantSet() {
    if (!Persist)
        return;

    TError error = Storage->RemoveNode(Id);
    if (error)
        L_ERR() << "Can't remove key-value node " << Id << ": " << error << std::endl;
}

TVariantSet::TVariantSet(std::shared_ptr<TKeyValueStorage> storage,
            std::shared_ptr<TRawValueMap> values,
            const std::string &id,
            bool persist) :
    Storage(storage), Values(values), Id(id), Persist(persist) { }

TError TVariantSet::Create() {
    if (!Persist)
        return TError::Success();

    return Storage->Create(Id);
}

TError TVariantSet::Restore(const kv::TNode &node) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        TError error = Values->IsValid(key);
        if (error)
            continue;

        auto *av = (*Values)[key];
        if (!(av->GetFlags() & PERSISTENT_VALUE))
            continue;

        if (config().log().verbose())
            L() << "Restoring " << key << " = " << value << std::endl;

        error = av->FromString(value);
        if (error)
            L_ERR() << error << ": Can't restore " << key << ", skipped" << std::endl;
    }

    return TError::Success();
}

std::vector<std::string> TVariantSet::List() {
    return Values->List();
}

bool TVariantSet::IsDefault(const std::string &name) const {
    return Values->IsDefault(name);
}

bool TVariantSet::HasValue(const std::string &name) const {
    return Values->HasValue(name);
}

void TVariantSet::Reset(const std::string &name) {
    (*Values)[name]->Reset();
}

TError TVariantSet::Flush() {
    if (!Persist)
        return TError::Success();

    return Storage->Create(Id);
}

TError TVariantSet::Sync() {
    if (!Persist)
        return TError::Success();

    kv::TNode node;

    for (auto name : Values->List()) {
        auto *av = (*Values)[name];

        if (!(av->GetFlags() & PERSISTENT_VALUE))
            continue;

        if (Values->IsDefault(name))
            continue;

        auto pair = node.add_pairs();
        pair->set_key(name);
        pair->set_val(ToString(name));

        if (config().log().verbose())
            L() << "Sync " << name << " = " << ToString(name) << std::endl;
    }

    return Storage->AppendNode(Id, node);
}

std::string TVariantSet::ToString(const std::string &name) const {
    return (*Values)[name]->ToString();
}
TError TVariantSet::FromString(const std::string &name, const std::string &value) {
    bool resetOnDefault = HasValue(name) && (*Values)[name]->DefaultString() == value;

    TError error = (*Values)[name]->FromString(value);
    if (error)
        return error;

    if ((*Values)[name]->GetFlags() & PERSISTENT_VALUE) {
        error = Storage->Append(Id, name, value);
    }

    if (resetOnDefault)
        (*Values)[name]->Reset();

    return error;
}
