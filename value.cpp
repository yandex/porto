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

bool TValueDef::GetDefaultBool(std::shared_ptr<TContainer> c) {
    return false;
}

TError TValueDef::SetBool(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TValueState> s,
                       const bool value) {
    return TError::Success();
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
        return TError(EError::Unknown, "Invalid property");
    };
}

TValueState::TValueState(std::shared_ptr<TContainer> c, TValueDef *p) : Property(p), Container(c) { }

std::string TValueState::GetStr() {
    /*
    if (!Property)
        return StringVal;
        */

    if (!Initialized) {
        auto c = Container.lock();
        PORTO_ASSERT(c);
        switch (Property->Type) {
        case EValueType::String:
            return Property->GetDefaultString(c);
        case EValueType::Bool:
            return BoolToStr(Property->GetDefaultBool(c));
        }
        // TODO: ^^^ allow only str
    }

    switch (Property->Type) {
    case EValueType::String:
        return StringVal;
    case EValueType::Bool:
        return BoolToStr(GetBool());
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

    TError error = Property->Set(c, shared_from_this(), v);
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
    PORTO_ASSERT(Property->Type == EValueType::Bool);

    if (!Initialized) {
        auto c = Container.lock();
        PORTO_ASSERT(c);
        return Property->GetDefaultBool(c);
    }

    return BoolVal;
}

bool TValueState::IsDefault() {
    if (!Initialized)
        return true;

    auto c = Container.lock();
    if (!c) {
        TError error(EError::Unknown, "Can't convert weak container reference");
        TLogger::LogError(error, "Can't check whether property is default");
        return false;
    }

    switch (Property->Type) {
    case EValueType::String:
        return StringVal == Property->GetDefaultString(c);
    case EValueType::Bool:
        return BoolVal == Property->GetDefaultBool(c);
    default:
        return false;
    };
}

extern std::map<std::string, TValueDef *> propSpec;
std::shared_ptr<TValueState> TValueHolder::operator[](const std::string &property) {
    auto c = Container.lock();
    if (!c) {
        TError error(EError::Unknown, "Can't convert weak container reference");
        TLogger::LogError(error, "Can't get property " + property);
        return nullptr;
    }

    if (State.find(property) == State.end()) {
        TValueDef *p = nullptr;
        if (propSpec.find(property) == propSpec.end()) {
            TLogger::Log() << "Error: invalid property " << property << std::endl;
            return nullptr;
        }
        PORTO_ASSERT(propSpec.find(property) != propSpec.end());
        p = propSpec.at(property);
        State[property] = std::make_shared<TValueState>(c, p);
    }

    return State.at(property);
}

bool TValueHolder::IsDefault(const std::string &property) {
    if (State.find(property) == State.end())
        return true;

    return State.at(property)->IsDefault();
}
