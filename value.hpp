#ifndef __VALUE_HPP__
#define __VALUE_HPP__

#include <string>
#include <map>
#include <memory>

#include "error.hpp"
#include "porto.hpp"

class TContainer;

enum class EValueType {
    // any string
    String,
    // key: val; key: val
    Map,
    // key; key; key
    List,

#if 0
    // TODO: ?string encoded int
    Int,
    UInt,
    // TODO: ?bool
    Bool
#endif
};

class TValueDef {
    NO_COPY_CONSTRUCT(TValueDef);

public:
    TValueDef(const std::string &name,
              const EValueType type,
              const std::string &desc,
              int flags = 0) : Name(name), Type(type), Desc(desc), Flags(flags) {}

    const std::string Name;
    const EValueType Type;
    const std::string Desc;
    const int Flags;

    virtual std::string GetDefaultString(std::shared_ptr<TContainer> c);
    virtual TError IsValidString(std::shared_ptr<TContainer> c,
                                 const std::string &value);

    virtual std::map<std::string, std::string>
        GetDefaultMap(std::shared_ptr<TContainer> c);
    virtual TError IsValidMap(std::shared_ptr<TContainer> c,
                              const std::map<std::string, std::string> &value);

    virtual std::vector<std::string>
        GetDefaultList(std::shared_ptr<TContainer> c);
    virtual TError IsValidList(std::shared_ptr<TContainer> c,
                               const std::vector<std::string> &value);


    virtual std::string GetDefault(std::shared_ptr<TContainer> c);
    virtual TError IsValid(std::shared_ptr<TContainer> c,
                           const std::string &value);
};

class TValueState {
    NO_COPY_CONSTRUCT(TValueState);

    TValueDef *Property;
    std::string StringVal;
    std::map<std::string, std::string> MapVal;
    std::vector<std::string> ListVal;
//    bool BoolVal;

public:
    TValueState(TValueDef *p, const std::string &v) : Property(p), StringVal(v) {}
    std::string Get();
};

#endif
