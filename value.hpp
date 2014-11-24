#ifndef __VALUE_HPP__
#define __VALUE_HPP__

#include <string>
#include <map>
#include <memory>
#include <set>

#include "error.hpp"
#include "porto.hpp"
#include "kvalue.hpp"
#include "util/log.hpp"

class TContainer;
enum class EContainerState;
class TTaskEnv;

enum class EValueType {
    String,
    Bool,
    Int,
    Uint,
    Map,
    List,
};

// Don't return default value, call get handler
const unsigned int NODEF_VALUE = (1 << 31);
// Value is not shown in the property/data list
const unsigned int HIDDEN_VALUE = (1 << 30);
// Value should be preserved upon recovery
const unsigned int PERSISTENT_VALUE = (1 << 29);

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

typedef std::map<std::string, uint64_t> TUintMap;
typedef std::vector<std::string> TStrList;

#define DEFINE_TVALUE(NAME, TYPE) \
virtual TYPE GetDefault ## NAME(std::shared_ptr<TContainer> c) { \
    return TYPE{}; \
} \
virtual TError Parse ## NAME(std::shared_ptr<TContainer> c,\
                             const TYPE &value) { \
    ExpectType(EValueType::NAME); \
    return TError::Success(); \
} \
virtual TError Set ## NAME(std::shared_ptr<TContainer> c, \
                           std::shared_ptr<TVariant> v, \
                           const TYPE &value) { \
    ExpectType(EValueType::NAME); \
    TError error = Parse ## NAME(c, value); \
    if (error) \
        return error; \
    return v->Set(EValueType::NAME, value); \
} \
virtual TYPE Get ## NAME(std::shared_ptr<TContainer> c, \
                         std::shared_ptr<TVariant> v) { \
    ExpectType(EValueType::NAME); \
    if (!v->HasValue() && NeedDefault()) \
        return GetDefault ## NAME(c); \
    return v->Get<TYPE>(Type); \
}

class TValue {
    NO_COPY_CONSTRUCT(TValue);

protected:
    void ExpectType(EValueType type);
    bool NeedDefault();
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

    virtual std::string GetDefaultString(std::shared_ptr<TContainer> c) = 0;
    virtual TError SetString(std::shared_ptr<TContainer> c,
                             std::shared_ptr<TVariant> v,
                             const std::string &value) = 0;
    virtual std::string GetString(std::shared_ptr<TContainer> c,
                                  std::shared_ptr<TVariant> v) = 0;
    virtual TError ParseString(std::shared_ptr<TContainer> c,
                               const std::string &value) {
        return TError::Success();
    }
    virtual TError ParseDefault(std::shared_ptr<TContainer> c) = 0;

    DEFINE_TVALUE(Bool, bool)
    DEFINE_TVALUE(Int, int)
    DEFINE_TVALUE(Uint, uint64_t)
    DEFINE_TVALUE(Map, TUintMap)
    DEFINE_TVALUE(List, TStrList)

    virtual TError PrepareTaskEnv(std::shared_ptr<TContainer> container,
                                  std::shared_ptr<TTaskEnv> taskEnv);
};

#undef DEFINE_TVALUE

#define VALUE_CLASS(NAME, TYPE) \
std::string NAME ## ToString(const TYPE &v); \
class T ## NAME ## Value : public TValue { \
public: \
    T ## NAME ## Value(const std::string &name, \
                 const std::string &desc, \
                 const int flags, \
                 const std::set<EContainerState> &state) : \
        TValue(name, EValueType::NAME, desc, flags, state) {} \
    std::string GetDefaultString(std::shared_ptr<TContainer> c); \
    TError SetString(std::shared_ptr<TContainer> c, \
                     std::shared_ptr<TVariant> v, \
                     const std::string &value); \
    std::string GetString(std::shared_ptr<TContainer> c, \
                          std::shared_ptr<TVariant> v); \
    bool IsDefault(std::shared_ptr<TContainer> c, \
                   std::shared_ptr<TVariant> v) { \
        if (!NeedDefault()) \
            return false; \
        if (!v->HasValue()) \
            return true; \
        return v->Get<TYPE>(EValueType::NAME) == GetDefault ## NAME(c); \
    } \
    virtual TError ParseDefault(std::shared_ptr<TContainer> c) { \
        return Parse ## NAME(c, GetDefault ## NAME (c)); \
    } \
}

VALUE_CLASS(String, std::string);
VALUE_CLASS(Bool, bool);
VALUE_CLASS(Int, int);
VALUE_CLASS(Uint, uint64_t);
VALUE_CLASS(Map, TUintMap);
VALUE_CLASS(List, TStrList);

#undef VALUE_CLASS

class TValueSet {
    std::map<std::string, TValue *> Value;
public:
    TError Register(TValue *p);
    TError Register(const std::vector<TValue *> &v);
    bool Valid(const std::string &name);
    TValue *Get(const std::string &name);
    std::vector<std::string> GetNames();
    std::string Overlap(TValueSet &other);
};

#define SYNTHESIZE_ACCESSOR(NAME, TYPE) \
    TYPE Get ## NAME(const std::string &name) { \
        TValue *p = nullptr; \
        std::shared_ptr<TContainer> c; \
        std::shared_ptr<TVariant> v; \
        TError error = Get(name, c, &p, v); \
        if (error) \
            L_ERR() << "Can't get value " << name << ": " << error << std::endl; \
        return p->Get ## NAME(c, v); \
    } \
    TError Set ## NAME(const std::string &name, const TYPE &value) { \
        TValue *p = nullptr; \
        std::shared_ptr<TContainer> c; \
        std::shared_ptr<TVariant> v; \
        TError error = Get(name, c, &p, v); \
        if (error) \
            return error; \
        if (p->Flags & PERSISTENT_VALUE) { \
            error = AppendStorage(name, NAME ## ToString(value)); \
            if (error) \
                return error; \
        } \
        return p->Set ## NAME(c, v, value); \
    }

class TVariantSet {
    NO_COPY_CONSTRUCT(TVariantSet);
    std::shared_ptr<TKeyValueStorage> Storage;
    TValueSet *ValueSet;
    std::weak_ptr<TContainer> Container;
    std::string Name;
    std::map<std::string, std::shared_ptr<TVariant>> Variant;

    TError AppendStorage(const std::string& key, const std::string& value);
    bool IsRoot();

public:
    TError Get(const std::string &name, std::shared_ptr<TContainer> &c,
               TValue **p, std::shared_ptr<TVariant> &v);

    TVariantSet(std::shared_ptr<TKeyValueStorage> storage,
                TValueSet *v, std::shared_ptr<TContainer> c);
    ~TVariantSet();

    TError Create();
    TError Restore(const kv::TNode &node);

    SYNTHESIZE_ACCESSOR(String, std::string)
    SYNTHESIZE_ACCESSOR(Bool, bool)
    SYNTHESIZE_ACCESSOR(Int, int)
    SYNTHESIZE_ACCESSOR(Uint, uint64_t)
    SYNTHESIZE_ACCESSOR(List, TStrList)
    SYNTHESIZE_ACCESSOR(Map, TUintMap)

    std::vector<std::string> List();
    bool IsDefault(const std::string &name);
    bool HasValue(const std::string &name);

    TError Flush();
    TError Sync();
};

#undef SYNTHESIZE_ACCESSOR

#endif
