#ifndef __VALUE_HPP__
#define __VALUE_HPP__

#include <string>
#include <map>
#include <memory>

#include "error.hpp"
#include "porto.hpp"

class TContainer;

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

class TValueState;

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
    virtual TError SetString(std::shared_ptr<TContainer> c,
                             std::shared_ptr<TValueState> s,
                             const std::string &value);

    virtual bool GetDefaultBool(std::shared_ptr<TContainer> c);
    virtual TError SetBool(std::shared_ptr<TContainer> c,
                           std::shared_ptr<TValueState> s,
                           const bool value);

#if 0
    virtual std::map<std::string, std::string>
        GetDefaultMap(std::shared_ptr<TContainer> c,
                      std::shared_ptr<TValueState> s);
    virtual TError SetMap(std::shared_ptr<TContainer> c,
                              std::shared_ptr<TValueState> s,
                              const std::map<std::string, std::string> &value);

    virtual std::vector<std::string>
        GetDefaultList(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TValueState> s);
    virtual TError SetList(std::shared_ptr<TContainer> c,
                               std::shared_ptr<TValueState> s,
                               const std::vector<std::string> &value);
#endif

    virtual std::string GetDefault(std::shared_ptr<TContainer> c);
    virtual TError Set(std::shared_ptr<TContainer> c,
                       std::shared_ptr<TValueState> s,
                       const std::string &value);
};

class TValueSpec {
    std::map<std::string, TValueDef *> Spec;
public:
    TError Register(TValueDef *p);
    bool Valid(const std::string &name);
    TValueDef *Get(const std::string &name);
    std::vector<std::string> GetNames();
};

class TValueState : public std::enable_shared_from_this<TValueState> {
    NO_COPY_CONSTRUCT(TValueState);

    friend TValueDef;

    TValueDef *Def;
    std::weak_ptr<TContainer> Container;
    std::string StringVal = "";
    bool BoolVal = false;
//    std::map<std::string, std::string> MapVal;
//    std::vector<std::string> ListVal;

    bool Initialized = false;

public:
    TValueState(std::shared_ptr<TContainer> c, TValueDef *p);
    bool IsDefault();
    std::string GetStr();
    TError SetStr(const std::string &v);
    void SetRawStr(const std::string &v);
    bool GetBool();


    // TODO: add Get/Set which converts everything to string
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
