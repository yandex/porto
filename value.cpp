#include "value.hpp"
#include "util/log.hpp"

#define PORTO_RUNTIME_ERROR(MSG) \
    do { \
        TLogger::Log() << "Runtime error: " << (MSG) << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        abort(); \
    } while (0)

static std::string BoolToStr(bool v) {
    if (v)
        return "true";
    else
        return "false";
}

void TValueDef::ExpectType(EValueType type) {
    if (type != Type)
        PORTO_RUNTIME_ERROR("Invalid type for " + Name);
}

std::string TValueDef::GetDefaultString(std::shared_ptr<TContainer> c) {
    ExpectType(EValueType::String);
    return "";
}

TError TValueDef::SetString(std::shared_ptr<TContainer> c,
                            std::shared_ptr<TValueState> s,
                            const std::string &value) {
    ExpectType(EValueType::String);
    return TError::Success();
}

std::string TValueDef::GetString(std::shared_ptr<TContainer> c,
                                 std::shared_ptr<TValueState> s) {
    ExpectType(EValueType::String);
    return s->StringVal;
}

bool TValueDef::GetDefaultBool(std::shared_ptr<TContainer> c) {
    ExpectType(EValueType::Bool);
    return false;
}

TError TValueDef::SetBool(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TValueState> s,
                       const bool value) {
    ExpectType(EValueType::Bool);
    return TError::Success();
}

bool TValueDef::GetBool(std::shared_ptr<TContainer> c,
                        std::shared_ptr<TValueState> s) {
    ExpectType(EValueType::Bool);
    return s->BoolVal;
}

std::string TStringValue::GetDefault(std::shared_ptr<TContainer> c) {
    return GetDefaultString(c);
}

TError TStringValue::Set(std::shared_ptr<TContainer> c,
                         std::shared_ptr<TValueState> s,
                         const std::string &value) {
    TError error = SetString(c, s, value);
    if (error)
        return error;

    s->StringVal = value;
    return TError::Success();
}

std::string TStringValue::Get(std::shared_ptr<TContainer> c,
                              std::shared_ptr<TValueState> s) {
    return GetString(c, s);
}

std::string TBoolValue::GetDefault(std::shared_ptr<TContainer> c) {
    return BoolToStr(GetDefaultBool(c));
}

TError TBoolValue::Set(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TValueState> s,
                       const std::string &value) {
    if (value != "true" && value != "false")
        return TError(EError::InvalidValue, "invalid boolean value");

    bool tmp = value == "true";

    TError error = SetBool(c, s, tmp);
    if (error)
        return error;

    s->BoolVal = tmp;
    return TError::Success();
}

std::string TBoolValue::Get(std::shared_ptr<TContainer> c,
                            std::shared_ptr<TValueState> s) {
    return BoolToStr(GetBool(c, s));
}

TValueState::TValueState(std::shared_ptr<TContainer> c, TValueDef *p) : Def(p), Container(c) { }

bool TValueState::ReturnDefault() {
    if (Def->Flags & NODEF_VALUE)
        return false;

    return !Initialized;
}

std::string TValueState::Get() {
    auto c = Container.lock();
    PORTO_ASSERT(c);

    if (ReturnDefault())
        return Def->GetDefault(c);

    return Def->Get(c, shared_from_this());
}

std::string TValueState::GetString() {
    PORTO_ASSERT(Def->Type == EValueType::String);
    return Get();
}

TError TValueState::Set(const std::string &v) {
    auto c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    TError error = Def->Set(c, shared_from_this(), v);
    if (error)
        return error;

    Initialized = true;

    return TError::Success();
}

TError TValueState::SetString(const std::string &v) {
    PORTO_ASSERT(Def->Type == EValueType::String);
    return Set(v);
}

void TValueState::SetRaw(const std::string &v) {
    StringVal = v;
    Initialized = true;
}

bool TValueState::GetBool() {
    PORTO_ASSERT(Def->Type == EValueType::Bool);

    auto c = Container.lock();
    PORTO_ASSERT(c);

    if (ReturnDefault())
        return Def->GetDefaultBool(c);

    return Def->GetBool(c, shared_from_this());
}

TError TValueState::SetBool(const bool &v) {
    PORTO_ASSERT(Def->Type == EValueType::Bool);

    auto c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    TError error = Def->SetBool(c, shared_from_this(), v);
    if (error)
        return error;

    Initialized = true;

    return TError::Success();
}

bool TValueState::IsDefault() {
    if (ReturnDefault())
        return true;

    auto c = Container.lock();
    if (!c) {
        TError error(EError::Unknown, "Can't convert weak container reference");
        TLogger::LogError(error, "Can't check whether value is default");
        return false;
    }

    return Def->Get(c, shared_from_this()) == Def->GetDefault(c);
}

TError TValueHolder::Get(const std::string &name, std::shared_ptr<TValueState> &s) {
    auto c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    if (State.find(name) == State.end()) {
        TValueDef *p = nullptr;
        if (!Spec->Valid(name))
            return TError(EError::Unknown, "Invalid value " + name);
        p = Spec->Get(name);
        State[name] = std::make_shared<TValueState>(c, p);
    }

    s = State.at(name);
    return TError::Success();
}

bool TValueHolder::IsDefault(const std::string &name) {
    if (State.find(name) == State.end())
        return true;

    return State.at(name)->IsDefault();
}

TError TValueSpec::Register(TValueDef *p) {
    if (Spec.find(p->Name) != Spec.end())
        return TError(EError::Unknown, "Invalid " + p->Name + " definition");
    Spec[p->Name] = p;
    return TError::Success();
}

TError TValueSpec::Register(const std::vector<TValueDef *> &v) {
    for (auto &p : v) {
        TError error = Register(p);
        if (error)
            return error;
    }
    return TError::Success();
}

bool TValueSpec::Valid(const std::string &name) {
    return Spec.find(name) != Spec.end();
}

TValueDef *TValueSpec::Get(const std::string &name) {
    return Spec[name];
}

std::vector<std::string> TValueSpec::GetNames() {
    std::vector<std::string> v;
    for (auto kv: Spec)
        v.push_back(kv.first);
    return v;
}
