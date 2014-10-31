#ifndef __VALUE_HPP__
#define __VALUE_HPP__

#include <string>
#include <map>
#include <memory>
#include <set>

#include "error.hpp"
#include "porto.hpp"
#include "util/log.hpp"

class TContainer;
enum class EContainerState;

enum class EValueType {
    String,
    Bool,

#if 0
    Int,
    UInt,
    // key: val; key: val
    Map,
    // key; key; key
    List,
#endif
};

// Don't return default value, call get handler
const unsigned int NODEF_VALUE = (1 << 31);
// Value is not shown in the property/data list
const unsigned int HIDDEN_VALUE = (1 << 30);

class TVariant {
    NO_COPY_CONSTRUCT(TVariant);

    struct TValueAbstractImpl {
        virtual ~TValueAbstractImpl() {}
    };

    template<typename T>
    struct TValueImpl : TValueAbstractImpl {
        T Value;
        TValueImpl(T value) : Value(value) {}
    };

    std::shared_ptr<TValueAbstractImpl> Impl;

public:
    const EValueType Type;
    const std::string Name;

    TVariant(const EValueType type, const std::string &name) :
        Type(type), Name(name) {}

    bool HasValue() { return Impl != nullptr; }

    template<typename T>
    const T Get(const EValueType type) {
        if (!Impl)
            PORTO_RUNTIME_ERROR("Invalid variant " + Name + " get: nullptr");

        if (Type != type)
            PORTO_RUNTIME_ERROR("Invalid variant " + Name + " get type: " +
                                std::to_string((int)Type) + " != " +
                                std::to_string((int)type));

        auto p = static_cast<TValueImpl<T> *>(Impl.get());
        return p->Value;
    }

    template<typename T>
    TError Set(const EValueType type, const T &value) {
        if (Type != type)
            PORTO_RUNTIME_ERROR("Invalid variant " + Name + " get type: " +
                                std::to_string((int)Type) + " != " +
                                std::to_string((int)type));

        Impl = std::make_shared<TValueImpl<T>>(value);
        return TError::Success();
    };
};

class TValue {
    NO_COPY_CONSTRUCT(TValue);

protected:
    void ExpectType(EValueType type);
public:
    TValue(const std::string &name,
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

    virtual bool IsDefault(std::shared_ptr<TContainer> c,
                           std::shared_ptr<TVariant> v) = 0;

    virtual std::string GetDefaultString(std::shared_ptr<TContainer> c);
    virtual TError SetString(std::shared_ptr<TContainer> c,
                             std::shared_ptr<TVariant> v,
                             const std::string &value);
    virtual std::string GetString(std::shared_ptr<TContainer> c,
                                  std::shared_ptr<TVariant> v);

    virtual bool GetDefaultBool(std::shared_ptr<TContainer> c);
    virtual TError SetBool(std::shared_ptr<TContainer> c,
                           std::shared_ptr<TVariant> v,
                           const bool value);
    virtual bool GetBool(std::shared_ptr<TContainer> c,
                         std::shared_ptr<TVariant> v);
};

#define SYNTHESIZE_DEFAULT(NAME, TYPE) \
    bool IsDefault(std::shared_ptr<TContainer> c, \
                   std::shared_ptr<TVariant> v) { \
        if (!v->HasValue()) \
            return true; \
        return v->Get<TYPE>(EValueType::NAME) == GetDefault ## NAME(c); \
    }

class TStringValue : public TValue {
public:
    TStringValue(const std::string &name,
                 const std::string &desc,
                 const int flags,
                 const std::set<EContainerState> &state) :
        TValue(name, EValueType::String, desc, flags, state) {}

    SYNTHESIZE_DEFAULT(String, std::string)
};

class TBoolValue : public TValue {
    NO_COPY_CONSTRUCT(TBoolValue);

    std::string BoolToStr(bool v);

public:
    TBoolValue(const std::string &name,
               const std::string &desc,
               const int flags,
               const std::set<EContainerState> &state) :
        TValue(name, EValueType::Bool, desc, flags, state) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c);
    TError SetString(std::shared_ptr<TContainer> c,
                             std::shared_ptr<TVariant> v,
                             const std::string &value);
    std::string GetString(std::shared_ptr<TContainer> c,
                                  std::shared_ptr<TVariant> v);

    SYNTHESIZE_DEFAULT(Bool, bool)
};

#undef SYNTHESIZE_DEFAULT

class TValueSet {
    std::map<std::string, TValue *> Value;
public:
    TError Register(TValue *p);
    TError Register(const std::vector<TValue *> &v);
    bool Valid(const std::string &name);
    TValue *Get(const std::string &name);
    std::vector<std::string> GetNames();
};

#define SYNTHESIZE_ACCESSOR(NAME, TYPE) \
    TError Get ## NAME(const std::string &name, TYPE &value) { \
        TValue *p = nullptr; \
        std::shared_ptr<TContainer> c; \
        std::shared_ptr<TVariant> v; \
        TError error = Get(name, c, &p, v); \
        if (error) \
            return error; \
        value = p->Get ## NAME(c, v); \
        return TError::Success(); \
    } \
    TError Set ## NAME(const std::string &name, const TYPE &value) { \
        TValue *p = nullptr; \
        std::shared_ptr<TContainer> c; \
        std::shared_ptr<TVariant> v; \
        TError error = Get(name, c, &p, v); \
        if (error) \
            return error; \
        return p->Set ## NAME(c, v, value); \
    }

class TVariantSet {
    NO_COPY_CONSTRUCT(TVariantSet);
    TValueSet *ValueSet;
    std::weak_ptr<TContainer> Container;

public:
    TError Get(const std::string &name, std::shared_ptr<TContainer> &c,
               TValue **p, std::shared_ptr<TVariant> &v);

    // TODO: make Variant private
    std::map<std::string, std::shared_ptr<TVariant>> Variant;

    TVariantSet(TValueSet *v, std::weak_ptr<TContainer> c) :
        ValueSet(v), Container(c) {}

    SYNTHESIZE_ACCESSOR(String, std::string)
    SYNTHESIZE_ACCESSOR(Bool, bool)

    bool IsDefault(const std::string &name);
};

#undef SYNTHESIZE_ACCESSOR

#endif
