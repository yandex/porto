#include "value.hpp"
#include "util/log.hpp"

std::string BoolToStr(bool v) {
    if (v)
        return "true";
    else
        return "false";
}

std::string TValueDef::GetDefaultString(std::shared_ptr<TContainer> c) {
    return "";
}

TError TValueDef::SetString(std::shared_ptr<TContainer> c,
                            std::shared_ptr<TValueState> s,
                            const std::string &value) {
    return TError::Success();
}

std::string TValueDef::GetString(std::shared_ptr<TContainer> c,
                                 std::shared_ptr<TValueState> s) {
    return s->StringVal;
}

bool TValueDef::GetDefaultBool(std::shared_ptr<TContainer> c) {
    return false;
}

TError TValueDef::SetBool(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TValueState> s,
                       const bool value) {
    return TError::Success();
}

bool TValueDef::GetBool(std::shared_ptr<TContainer> c,
                        std::shared_ptr<TValueState> s) {
    return s->BoolVal;
}
#if 0
std::map<std::string, std::string>
TValueDef::GetDefaultMap(std::shared_ptr<TContainer> c,
                         std::shared_ptr<TValueState> s) {
    std::map<std::string, std::string> m;
    return m;
}

TError TValueDef::SetMap(std::shared_ptr<TContainer> c,
                             std::shared_ptr<TValueState> s,
                             const std::map<std::string, std::string> &value) {
    // TODO;
    return TError::Success();
}

std::vector<std::string>
TValueDef::GetDefaultList(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s) {
    std::vector<std::string> v;
    return v;
}

TError TValueDef::SetList(std::shared_ptr<TContainer> c,
                          std::shared_ptr<TValueState> s,
                          const std::vector<std::string> &value) {
    // TODO;
    return TError::Success();
}
#endif

std::string TValueDef::GetDefault(std::shared_ptr<TContainer> c) {
    switch (Type) {
    case EValueType::String:
        return GetDefaultString(c);
    case EValueType::Bool:
        return BoolToStr(GetDefaultBool(c));
#if 0
    case EValueType::Map:
        return GetDefaultMap(c);
    case EValueType::List:
        return GetDefaultList(c);
#endif
    default:
        return "";
    };
}

#include "container.hpp"
TError TValueDef::Set(std::shared_ptr<TContainer> c,
                      std::shared_ptr<TValueState> s,
                      const std::string &value) {
    TError error;
    bool tmpBool;

    switch (Type) {
    case EValueType::String:
        error = SetString(c, s, value);
        if (error)
            return error;

        // TODO SetStr
        s->StringVal = value;
        return TError::Success();

    case EValueType::Bool:
        if (value != "true" && value != "false")
            return TError(EError::InvalidValue, "invalid boolean value");

        tmpBool = value == "true";

        error = SetBool(c, s, tmpBool);
        if (error)
            return error;

        s->BoolVal = tmpBool;
        return TError::Success();
#if 0
    // TODO: parse value
    case EValueType::Map:
        return SetMap(c, s, value);
    case EValueType::List:
        return SetList(c, s, value);
#endif
    default:
        return TError(EError::Unknown, "Invalid value name");
    };
}

TValueState::TValueState(std::shared_ptr<TContainer> c, TValueDef *p) : Def(p), Container(c) { }

bool TValueState::ReturnDefault() {
    if (Def->Flags & NODEF_VALUE)
        return false;

    return !Initialized;
}

std::string TValueState::GetStr() {
    auto c = Container.lock();
    PORTO_ASSERT(c);

    if (ReturnDefault()) {
        switch (Def->Type) {
        case EValueType::String:
            return Def->GetDefaultString(c);
        case EValueType::Bool:
            return BoolToStr(Def->GetDefaultBool(c));
        }
        // TODO: ^^^ allow only str
    }

    switch (Def->Type) {
    case EValueType::String:
        return Def->GetString(c, shared_from_this());
    case EValueType::Bool:
        return BoolToStr(Def->GetBool(c, shared_from_this()));
#if 0
    case EValueType::Map:
        // TODO:
        return MapVal;
    case EValueType::List:
        // TODO:
        return ListVal;
#endif
    default:
        return "";
    };
}

TError TValueState::SetStr(const std::string &v) {
    auto c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    TError error = Def->Set(c, shared_from_this(), v);
    if (error)
        return error;
    SetRawStr(v);
    return TError::Success();
}

void TValueState::SetRawStr(const std::string &v) {
    // TODO: ? assert type

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

bool TValueState::IsDefault() {
    if (ReturnDefault())
        return true;

    auto c = Container.lock();
    if (!c) {
        TError error(EError::Unknown, "Can't convert weak container reference");
        TLogger::LogError(error, "Can't check whether value is default");
        return false;
    }

    switch (Def->Type) {
    case EValueType::String:
        return Def->GetString(c, shared_from_this()) == Def->GetDefaultString(c);
    case EValueType::Bool:
        return Def->GetBool(c, shared_from_this()) == Def->GetDefaultBool(c);
    default:
        return false;
    };
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
