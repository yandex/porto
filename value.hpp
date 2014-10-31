#ifndef __VALUE_HPP__
#define __VALUE_HPP__

#include <string>
#include <map>
#include <memory>
#include <set>

#include "error.hpp"
#include "porto.hpp"

// TValueDef -> TValue
// TValueSpec -> TValueSet
//
// TValueState -> TValueState
// TValueHolder -> TValueStateSet

class TContainer;
enum class EContainerState;

enum class EValueType {
    String,
    Bool,

#if 0
    // key: val; key: val
    Map,
    // key; key; key
    List,

    // TODO: ?string encoded int
    Int,
    UInt,
#endif
};

// Don't return default value, call get handler
const unsigned int NODEF_VALUE = (1 << 31);
// Value is not shown in the property/data list
const unsigned int HIDDEN_VALUE = (1 << 30);

class TValueState;

class TValueDef {
    NO_COPY_CONSTRUCT(TValueDef);

protected:
    void ExpectType(EValueType type);

public:
    TValueDef(const std::string &name,
              const EValueType type,
              const std::string &desc,
              const int flags,
              const std::set<EContainerState> &state) :
        Name(name), Type(type), Desc(desc), Flags(flags), State(state) {}

    const std::string Name;
    const EValueType Type;
    const std::string Desc;
    const int Flags;
    const std::set<EContainerState> State;

    virtual std::string GetDefaultString(std::shared_ptr<TContainer> c);
    virtual TError SetString(std::shared_ptr<TContainer> c,
                             std::shared_ptr<TValueState> s,
                             const std::string &value);
    virtual std::string GetString(std::shared_ptr<TContainer> c,
                                  std::shared_ptr<TValueState> s);

    virtual bool GetDefaultBool(std::shared_ptr<TContainer> c);
    virtual TError SetBool(std::shared_ptr<TContainer> c,
                           std::shared_ptr<TValueState> s,
                           const bool value);
    virtual bool GetBool(std::shared_ptr<TContainer> c,
                         std::shared_ptr<TValueState> s);

    virtual std::string GetDefault(std::shared_ptr<TContainer> c) = 0;
    virtual TError Set(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TValueState> s,
                       const std::string &value) = 0;
    virtual std::string Get(std::shared_ptr<TContainer> c,
                            std::shared_ptr<TValueState> s) = 0;
};

class TStringValue : public TValueDef {
    NO_COPY_CONSTRUCT(TStringValue);
public:
    TStringValue(const std::string &name,
              const std::string &desc,
              const int flags,
              const std::set<EContainerState> &state) :
        TValueDef(name, EValueType::String, desc, flags, state) {}

    std::string GetDefault(std::shared_ptr<TContainer> c);
    TError Set(std::shared_ptr<TContainer> c,
               std::shared_ptr<TValueState> s,
               const std::string &value);
    std::string Get(std::shared_ptr<TContainer> c,
                    std::shared_ptr<TValueState> s);
};

class TBoolValue : public TValueDef {
    NO_COPY_CONSTRUCT(TBoolValue);
public:
    TBoolValue(const std::string &name,
               const std::string &desc,
               const int flags,
               const std::set<EContainerState> &state) :
        TValueDef(name, EValueType::Bool, desc, flags, state) {}

    std::string GetDefault(std::shared_ptr<TContainer> c);
    TError Set(std::shared_ptr<TContainer> c,
               std::shared_ptr<TValueState> s,
               const std::string &value);
    std::string Get(std::shared_ptr<TContainer> c,
                    std::shared_ptr<TValueState> s);
};

class TValueSpec {
    std::map<std::string, TValueDef *> Spec;
public:
    TError Register(TValueDef *p);
    TError Register(const std::vector<TValueDef *> &v);
    bool Valid(const std::string &name);
    TValueDef *Get(const std::string &name);
    std::vector<std::string> GetNames();
};

class TValueState : public std::enable_shared_from_this<TValueState> {
    NO_COPY_CONSTRUCT(TValueState);

public:
    TValueDef *Def;
    std::weak_ptr<TContainer> Container;
    std::string StringVal = "";
    bool BoolVal = false;

    bool Initialized = false;
    bool ReturnDefault();

    TValueState(std::shared_ptr<TContainer> c, TValueDef *p);
    bool IsDefault();

    std::string Get();
    TError Set(const std::string &v);
    void SetRaw(const std::string &v);

    std::string GetString();
    TError SetString(const std::string &v);
    bool GetBool();
    TError SetBool(const bool &v);
};

class TValueHolder {
    NO_COPY_CONSTRUCT(TValueHolder);
    TValueSpec *Spec;
    std::weak_ptr<TContainer> Container;

public:
    std::map<std::string, std::shared_ptr<TValueState>> State;

    TValueHolder(TValueSpec *spec, std::weak_ptr<TContainer> c) : Spec(spec), Container(c) {}
    TError Get(const std::string &name, std::shared_ptr<TValueState> &s);
    bool IsDefault(const std::string &name);
};

#endif
