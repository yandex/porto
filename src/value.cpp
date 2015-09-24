#include <sstream>

#include "value.hpp"
#include "kv.pb.h"
#include "config.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

bool TAbstractValue::HasValue() const {
    return Variant.HasValue();
}

void TAbstractValue::Reset() {
    Variant.Reset();
}

int TAbstractValue::GetFlags() const {
    return Flags;
}

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

std::string TDoubleValue::ToString(const double &value) const {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%lg", value);
    return std::string(buffer);
}

TError TDoubleValue::FromString(const std::string &value) {
    double tmp;
    TError error = StringToDouble(value, tmp);
    if (error)
        return TError(EError::InvalidValue, "Invalid unsigned integer value " + value);

    error = CheckValue(tmp);
    if (error)
        return error;

    return Set(tmp);
}

double TDoubleValue::GetDefault() const {
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

std::string TIntListValue::ToString(const std::vector<int> &value) const {
   std::stringstream str;
   bool first = true;

   for (auto v : value) {
      if (first)
         first = false;
      else
         str << ";";
      str << v;
   }

   return str.str();
}

TError TIntListValue::FromString(const std::string &value) {
   std::vector<std::string> strings;
   std::vector<int> integers;

   TError error = SplitEscapedString(value, ';', strings);
   if (error)
      return error;

   error = StringsToIntegers(strings, integers);
   if (error)
      return error;

   Set(integers);
   return TError::Success();
}

std::vector<int> TIntListValue::GetDefault() const {
    return std::vector<int>{};
}

std::string TListValue::ToString(const TStrList &value) const {
    std::stringstream str;

    for (auto v : value) {
        if (str.str().length())
            str << "; ";
        str << StringReplaceAll(v, ";", "\\;");
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
    if (AbstractValues.find(name) != AbstractValues.end())
        PORTO_RUNTIME_ERROR("Duplicate value");

    AbstractValues[name] = av;
    return TError::Success();
}

TAbstractValue *TRawValueMap::Find(const std::string &name) const {
    return AbstractValues.at(name);
}

bool TRawValueMap::IsValid(const std::string &name) const {
   return AbstractValues.find(name) != AbstractValues.end();
}

bool TRawValueMap::IsReadOnly(const std::string &name) const {
   return Find(name)->GetFlags() & READ_ONLY_VALUE;
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

TError TValueMap::Create() {
    if (!KvNode)
        return TError::Success();

    return KvNode->Create();
}

TError TValueMap::Remove() {
    if (!KvNode)
        return TError::Success();

    return KvNode->Remove();
}

TError TValueMap::Restore(const kv::TNode &node) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        if (!IsValid(key))
            continue;

        auto *av = Find(key);
        if (!(av->GetFlags() & PERSISTENT_VALUE))
            continue;

        if (config().log().verbose())
            L_ACT() << "Restoring " << key << " = " << value << std::endl;

        TError error = av->FromString(value);
        if (error)
            L_ERR() << error << ": Can't restore " << key << ", skipped" << std::endl;
    }

    return TError::Success();
}

TError TValueMap::Restore() {
    if (!KvNode)
        return TError::Success();

    kv::TNode n;
    TError error = KvNode->Load(n);
    if (error)
        return error;

    error = Restore(n);
    if (error)
        return error;

    error = Flush();
    if (error)
        return error;

    return Sync();
}

TError TValueMap::Flush() {
    if (!KvNode)
        return TError::Success();

    return KvNode->Create();
}

TError TValueMap::Sync() {
    if (!KvNode)
        return TError::Success();

    kv::TNode node;
    for (auto kv : AbstractValues) {
        auto name = kv.first;
        auto av = kv.second;

        if (!(av->GetFlags() & PERSISTENT_VALUE))
            continue;

        if (IsDefault(name))
            continue;

        auto pair = node.add_pairs();
        pair->set_key(name);
        pair->set_val(av->ToString());

        if (config().log().verbose())
            L_ACT() << "Sync " << name << " = " << av->ToString() << std::endl;
    }

    return KvNode->Append(node);
}

std::string TValueMap::ToString(const std::string &name) const {
    return Find(name)->ToString();
}

TError TValueMap::FromString(const std::string &name, const std::string &value, bool apply) {
    bool resetOnDefault = HasValue(name) && Find(name)->DefaultString() == value;

    TError error = Find(name)->FromString(value);
    if (error)
        return error;

    if (apply && KvNode && Find(name)->GetFlags() & PERSISTENT_VALUE)
        error = KvNode->Append(name, value);

    if (resetOnDefault)
        Find(name)->Reset();

    return error;
}

void TValueMap::Reset(const std::string &name) {
    Find(name)->Reset();
}
