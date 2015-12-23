#include <sstream>

#include "value.hpp"
#include "kv.pb.h"
#include "config.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

std::string TStringValue::ToString(const std::string &value) const {
    return value;
}

TError TStringValue::FromString(const std::string &value, std::string &result) const {
    result = value;
    return TError::Success();
}

std::string TStringValue::GetDefault() const {
    return "";
}

std::string TIntValue::ToString(const int &value) const {
    return std::to_string(value);
}

TError TIntValue::FromString(const std::string &value, int &result) const {
    if (StringToInt(value, result))
        return TError(EError::InvalidValue, "Invalid integer value " + value);
    return TError::Success();
}

int TIntValue::GetDefault() const {
    return 0;
}

std::string TUintValue::ToString(const uint64_t &value) const {
    return std::to_string(value);
}

TError TUintValue::FromString(const std::string &value, uint64_t &result) const {
    return StringToUint64(value, result);
}

uint64_t TUintValue::GetDefault() const {
    return 0;
}

std::string TSizeValue::ToString(const uint64_t &value) const {
    return std::to_string(value);
}

TError TSizeValue::FromString(const std::string &value, uint64_t &result) const {
    return StringToSize(value, result);
}

uint64_t TSizeValue::GetDefault() const {
    return 0;
}

std::string TDoubleValue::ToString(const double &value) const {
    return StringFormat("%lg", value);
}

TError TDoubleValue::FromString(const std::string &value, double &result) const {
    TError error = StringToDouble(value, result);
    if (error)
        return TError(EError::InvalidValue, "Invalid unsigned integer value " + value);

    return TError::Success();
}

double TDoubleValue::GetDefault() const {
    return 0;
}

std::string TBoolValue::ToString(const bool &value) const {
    if (value)
        return "true";
    return "false";
}

TError TBoolValue::FromString(const std::string &value, bool &result) const {
    if (value == "true")
        result = true;
    else if (value == "false")
        result = false;
    else
        return TError(EError::InvalidValue, "Invalid bool value");
    return TError::Success();
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

TError TIntListValue::FromString(const std::string &value, std::vector<int> &result) const {
   std::vector<std::string> strings;

   TError error = SplitEscapedString(value, ';', strings);
   if (error)
      return error;

   return StringsToIntegers(strings, result);
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

TError TListValue::FromString(const std::string &value, TStrList &result) const {
    std::vector<std::string> vec;
    TError error = SplitEscapedString(value, ';', vec);
    if (error)
        return error;

    for (auto &val : vec) {
        std::string tmp = StringTrim(val);
        if (!tmp.length())
            continue;
        result.push_back(tmp);
    }

    return TError::Success();
}

TStrList TListValue::GetDefault() const {
    return TStrList{};
}

std::string TMapValue::ToString(const TUintMap &value) const {
    return StringFormatUintMap(value);
}

TError TMapValue::FromString(const std::string &value, TUintMap &result) const {
    return StringToUintMap(value, result);
}

TUintMap TMapValue::GetDefault() const {
    return TUintMap{};
}

TError TMapValue::GetIndexed(const std::string &index, std::string &value) const {
    auto map = Get();
    auto it = map.find(index);

    if (it == map.end())
        return TError(EError::InvalidValue, "invalid index " + index);

    value = std::to_string(it->second);
    return TError::Success();
}

TError TMapValue::SetIndexed(const std::string &index, const std::string &value) {
    auto map = Get();
    uint64_t uval;
    TError error = StringToUint64(value, uval);
    if (error)
        return error;
    map[index] = uval;
    return Set(map);
}

TRawValueMap::~TRawValueMap() {
    for (auto pair : Values)
        delete pair.second;
}

TError TRawValueMap::Add(const std::string &name, TValue *av) {
    if (Values.find(name) != Values.end())
        PORTO_RUNTIME_ERROR("Duplicate value");

    Values[name] = av;
    return TError::Success();
}

TValue *TRawValueMap::Find(const std::string &name) const {
    auto it = Values.find(name);

    if (it == Values.end())
       return NULL;

    return it->second;
}

bool TRawValueMap::IsReadOnly(const std::string &name) const {
   return Find(name)->HasFlag(READ_ONLY_VALUE);
}

bool TRawValueMap::IsDefault(const std::string &name) const {
    return !HasValue(name);
}

bool TRawValueMap::HasValue(const std::string &name) const {
    TValue *p = Values.at(name);
    return p->HasValue();
}

std::vector<std::string> TRawValueMap::List() const {
    std::vector<std::string> v;
    for (auto pair : Values)
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

        auto *av = Find(key);
        if (!av || !av->HasFlag(PERSISTENT_VALUE))
            continue;

        if (config().log().verbose())
            L_ACT() << "Restoring " << key << " = " << value << std::endl;

        TError error = av->SetString(value);
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
    TError error;

    if (!KvNode)
        return TError::Success();

    kv::TNode node;
    for (auto kv : Values) {
        auto name = kv.first;
        auto av = kv.second;
        std::string value;

        if (!av->HasFlag(PERSISTENT_VALUE) || !av->HasValue())
            continue;

        error = av->GetString(value);
        if (error)
            return error;

        auto pair = node.add_pairs();
        pair->set_key(name);
        pair->set_val(value);

        if (config().log().verbose())
            L_ACT() << "Sync " << name << " = " << value << std::endl;
    }

    return KvNode->Append(node);
}

TError TValueMap::SetValue(const std::string &name, const std::string &value) {

    auto val = Find(name);
    if (!val)
       return TError(EError::InvalidValue, "Invalid value name: " + name);

    if (val->HasFlag(READ_ONLY_VALUE))
       return TError(EError::InvalidValue, "Read-only value: " + name);

    TError error = val->SetString(value);
    if (!error && KvNode && val->HasFlag(PERSISTENT_VALUE))
        error = KvNode->Append(name, value);

    return error;
}
