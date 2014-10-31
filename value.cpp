#include "value.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

void TValue::ExpectType(EValueType type) {
    if (type != Type)
        PORTO_RUNTIME_ERROR("Invalid " + Name + " type: " +
                            std::to_string((int)Type) + " != " +
                            std::to_string((int)type));
}

bool TValue::NeedDefault() {
    return !(Flags & NODEF_VALUE);
}

std::string TValue::GetDefaultString(std::shared_ptr<TContainer> c) {
    return "";
}

TError TValue::SetString(std::shared_ptr<TContainer> c,
                         std::shared_ptr<TVariant> v,
                         const std::string &value) {
    return v->Set(Type, value);
}

std::string TValue::GetString(std::shared_ptr<TContainer> c,
                              std::shared_ptr<TVariant> v) {
    if (!v->HasValue() && NeedDefault())
        return GetDefaultString(c);

    return v->Get<std::string>(Type);
}

bool TValue::GetDefaultBool(std::shared_ptr<TContainer> c) {
    ExpectType(EValueType::Bool);
    return false;
}

TError TValue::SetBool(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TVariant> v,
                       const bool value) {
    ExpectType(EValueType::Bool);
    return v->Set(Type, value);
}

bool TValue::GetBool(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TVariant> v) {
    ExpectType(EValueType::Bool);
    if (!v->HasValue() && NeedDefault())
        return GetDefaultBool(c);

    return v->Get<bool>(Type);
}

int TValue::GetDefaultInt(std::shared_ptr<TContainer> c) {
    ExpectType(EValueType::Int);
    return 0;
}

TError TValue::SetInt(std::shared_ptr<TContainer> c,
                      std::shared_ptr<TVariant> v,
                      const int value) {
    ExpectType(EValueType::Int);
    return v->Set(Type, value);
}

int TValue::GetInt(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TVariant> v) {
    ExpectType(EValueType::Int);
    if (!v->HasValue() && NeedDefault())
        return GetDefaultInt(c);

    return v->Get<int>(Type);
}

std::string TBoolValue::BoolToStr(bool v) {
    if (v)
        return "true";
    else
        return "false";
}

std::string TBoolValue::GetDefaultString(std::shared_ptr<TContainer> c) {
    return BoolToStr(GetDefaultBool(c));
}

TError TBoolValue::SetString(std::shared_ptr<TContainer> c,
                         std::shared_ptr<TVariant> v,
                         const std::string &value) {
    if (value != "true" && value != "false")
        return TError(EError::InvalidValue, "invalid boolean value");

    bool tmp = value == "true";

    return SetBool(c, v, tmp);
}
std::string TBoolValue::GetString(std::shared_ptr<TContainer> c,
                              std::shared_ptr<TVariant> v) {
    bool value;

    if (!v->HasValue() && NeedDefault())
        value = GetDefaultBool(c);
    else
        value = GetBool(c, v);

    return BoolToStr(value);
}

std::string TIntValue::GetDefaultString(std::shared_ptr<TContainer> c) {
    return std::to_string(GetDefaultInt(c));
}

TError TIntValue::SetString(std::shared_ptr<TContainer> c,
                            std::shared_ptr<TVariant> v,
                            const std::string &value) {
    int tmp;
    TError error = StringToInt(value, tmp);
    if (error)
        return TError(EError::InvalidValue, "Invalid integer value " + value);

    return SetInt(c, v, tmp);
}
std::string TIntValue::GetString(std::shared_ptr<TContainer> c,
                                 std::shared_ptr<TVariant> v) {
    int value;

    if (!v->HasValue() && NeedDefault())
        value = GetDefaultInt(c);
    else
        value = GetInt(c, v);

    return std::to_string(value);
}

TError TValueSet::Register(TValue *p) {
    if (Value.find(p->Name) != Value.end())
        return TError(EError::Unknown, "Invalid " + p->Name + " definition");
    Value[p->Name] = p;
    return TError::Success();
}

TError TValueSet::Register(const std::vector<TValue *> &v) {
    for (auto &p : v) {
        TError error = Register(p);
        if (error)
            return error;
    }
    return TError::Success();
}

bool TValueSet::Valid(const std::string &name) {
    return Value.find(name) != Value.end();
}

TValue *TValueSet::Get(const std::string &name) {
    return Value[name];
}

std::vector<std::string> TValueSet::GetNames() {
    std::vector<std::string> v;
    for (auto kv: Value)
        v.push_back(kv.first);
    return v;
}

TError TVariantSet::Get(const std::string &name,
                        std::shared_ptr<TContainer> &c, TValue **p,
                        std::shared_ptr<TVariant> &v) {
    c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    *p = nullptr;
    if (Variant.find(name) == Variant.end()) {
        if (!ValueSet->Valid(name))
            return TError(EError::Unknown, "Invalid value " + name);
        *p = ValueSet->Get(name);
        v = Variant[name] = std::make_shared<TVariant>((*p)->Type, name);
    } else {
        *p = ValueSet->Get(name);
        v = Variant[name];
    }

    return TError::Success();
}

std::vector<std::string> TVariantSet::List() {
    std::vector<std::string> v;
    for (auto kv : Variant)
        v.push_back(kv.first);
    return v;
}

bool TVariantSet::IsDefault(const std::string &name) {
    if (Variant.find(name) == Variant.end())
        return true;

    TValue *p = nullptr;
    std::shared_ptr<TContainer> c;
    std::shared_ptr<TVariant> v;
    TError error = Get(name, c, &p, v);
    if (error) {
        TLogger::LogError(error, "Can't check whether " + name + " is default");
        return false;
    }

    return p->IsDefault(c, v);
}
