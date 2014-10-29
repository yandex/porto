#include "value.hpp"

std::string TValueDef::GetDefaultString(std::shared_ptr<TContainer> c) {
    return "";
}

TError TValueDef::IsValidString(std::shared_ptr<TContainer> c,
                                const std::string &value) {
    return TError::Success();
}

std::map<std::string, std::string>
TValueDef::GetDefaultMap(std::shared_ptr<TContainer> c) {
    std::map<std::string, std::string> m;
    return m;
}

TError TValueDef::IsValidMap(std::shared_ptr<TContainer> c,
                             const std::map<std::string, std::string> &value) {
    // TODO;
    return TError::Success();
}

std::vector<std::string>
TValueDef::GetDefaultList(std::shared_ptr<TContainer> c) {
    std::vector<std::string> v;
    return v;
}

TError TValueDef::IsValidList(std::shared_ptr<TContainer> c,
                              const std::vector<std::string> &value) {
    // TODO;
    return TError::Success();
}


std::string TValueDef::GetDefault(std::shared_ptr<TContainer> c) {
    switch (Type) {
    case EValueType::String:
        return GetDefaultString(c);
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

TError TValueDef::IsValid(std::shared_ptr<TContainer> c,
                          const std::string &value) {
    // TODO: parse value

    switch (Type) {
    case EValueType::String:
        return IsValidString(c, value);
#if 0
    case EValueType::Map:
        return IsValidMap(c, value);
    case EValueType::List:
        return IsValidList(c, value);
#endif
    default:
        return TError(EError::Unknown, "Invalid property");
    };
}

std::string TValueState::Get() {
    if (!Property)
        return StringVal;

    switch (Property->Type) {
    case EValueType::String:
        return StringVal;
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
