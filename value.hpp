#pragma once

#include <string>
#include <map>
#include <memory>
#include <set>

#include "common.hpp"
#include "kvalue.hpp"
#include "util/log.hpp"


/// {{{ TEMP
#include <sstream>
#include "util/string.hpp"
/// }}} TEMP

class TContainer;
enum class EContainerState;
class TTaskEnv;

// Don't return default value, call get handler
const unsigned int NODEF_VALUE = (1 << 31);
// Value is not shown in the property/data list
const unsigned int HIDDEN_VALUE = (1 << 30);
// Value should be preserved upon recovery
const unsigned int PERSISTENT_VALUE = (1 << 29);
// Uint value can include options G/M/K suffix
const unsigned int UINT_UNIT_VALUE = (1 << 28);

class TVariant : public TNonCopyable {
    struct TValueAbstractImpl {
        virtual ~TValueAbstractImpl() {}
    };

    template<typename T>
    struct TValueImpl : TValueAbstractImpl {
        T Value;
        TValueImpl(T value) : Value(value) {}
    };

    std::shared_ptr<TValueAbstractImpl> Impl = nullptr;

public:
    TVariant() {}

    bool HasValue() const { return Impl != nullptr; }

    template<typename T>
    const T Get() const {
        if (!Impl)
            PORTO_RUNTIME_ERROR("Invalid variant get: nullptr");

        try {
            auto p = dynamic_cast<TValueImpl<T> *>(Impl.get());
            return p->Value;
        } catch (std::bad_cast &e) {
            PORTO_RUNTIME_ERROR(std::string("Invalid variant cast: ") + e.what());
            return {};
        }
    }

    template<typename T>
    TError Set(const T &value) {
        Impl = std::make_shared<TValueImpl<T>>(value);
        return TError::Success();
    };

    void Reset() { Impl = nullptr; }
};

template<typename T>
class TValue;

class TAbstractValue : public TNonCopyable {
protected:
    TVariant variant;
//    int Flags;
public:
    TAbstractValue() {}
    virtual ~TAbstractValue() {}

    virtual std::string ToString() const =0;
    virtual TError FromString(const std::string &value) =0;
    virtual std::string DefaultString() const =0;

    bool HasValue() { return variant.HasValue(); }
    void Reset() { variant.Reset(); }

    template<typename T>
    const T Get() {
        try {
            auto p = dynamic_cast<TValue<T> *>(this);
            return p->Get();
        } catch (std::bad_cast &e) {
            PORTO_RUNTIME_ERROR(std::string("Bad cast: ") + e.what());
            return {};
        }
    }

    template<typename T>
    TError Set(const T& value) {
        try {
            auto p = dynamic_cast<TValue<T> *>(this);
            return p->Set(value);
        } catch (std::bad_cast &e) {
            PORTO_RUNTIME_ERROR(std::string("Bad cast: ") + e.what());
            return {};
        }
    }
};

template<typename T>
class TValue : public TAbstractValue {
public:
    TValue() {}

    virtual T GetDefault() const =0;
    virtual std::string ToString(const T &value) const =0;
    virtual TError CheckValue(const T& value) {
        return TError::Success();
    }

    std::string ToString() const override {
        return ToString(Get());
    }

    std::string DefaultString() const override {
        return ToString(GetDefault());
    }

    const T Get() const {
        if (variant.HasValue())
            return variant.Get<T>();
        else
            return GetDefault();
    }

    TError Set(const T &value) {
        TError error = CheckValue(value);
        if (error)
            return error;

        variant.Set(value);
        return TError::Success();
    }
};

class TStringValue : public TValue<std::string> {
public:
    TStringValue() {}

    std::string ToString(const std::string &value) const override;
    TError FromString(const std::string &value) override;
    std::string GetDefault() const override;
};

class TIntValue : public TValue<int> {
public:
    TIntValue() {}

    std::string ToString(const int &value) const override;
    TError FromString(const std::string &value) override;
    int GetDefault() const override;
};

class TUintValue : public TValue<uint64_t> {
public:
    TUintValue() {}

    std::string ToString(const uint64_t &value) const override;
    TError FromString(const std::string &value) override;
    uint64_t GetDefault() const override;
};

class TBoolValue : public TValue<bool> {
public:
    TBoolValue() {}

    std::string ToString(const bool &value) const override;
    TError FromString(const std::string &value) override;
    bool GetDefault() const override;
};

typedef std::vector<std::string> TStrList;

class TListValue : public TValue<TStrList> {
public:
    TListValue() {}

    std::string ToString(const TStrList &value) const override;
    TError FromString(const std::string &value) override;
    TStrList GetDefault() const override;
};

typedef std::map<std::string, uint64_t> TUintMap;

class TMapValue : public TValue<TUintMap> {
public:
    TMapValue() {}

    std::string ToString(const TUintMap &value) const override;
    TError FromString(const std::string &value) override;
    TUintMap GetDefault() const override;
};

class TRawValueMap {
    std::map<std::string, TAbstractValue *> AbstractValues;

public:
    ~TRawValueMap();

    TError Add(const std::string &name, TAbstractValue *av);
    TAbstractValue *operator[](const std::string &name) const;
    TError IsValid(const std::string &name) const;
    bool IsDefault(const std::string &name) const;
    std::vector<std::string> List() const;
};


// TODO MOVE OUT OF THIS FILE {{{
class TContainerValue {
protected:
    const char *Name;
    const char *Desc;
    const int Flags;
    const std::set<EContainerState> State;
    std::weak_ptr<TContainer> Container;

    TContainerValue(
           const char *name,
           const char *desc,
           const int flags,
           const std::set<EContainerState> &state) :
        Name(name), Desc(desc), Flags(flags), State(state) {}

    std::shared_ptr<TContainer> GetContainer() const {
        std::shared_ptr<TContainer> container = Container.lock();
        PORTO_ASSERT(container);
        return container;
    }

public:
    void SetContainer(std::shared_ptr<TContainer> container) {
        Container = container;
    }
    const char *GetName() { return Name; }
    const char *GetDesc() { return Desc; }
    int GetFlags() { return Flags; }
    const std::set<EContainerState> &GetState() { return State; }
    virtual TError PrepareTaskEnv(std::shared_ptr<TTaskEnv> taskEnv) {
        return TError::Success();
    }
};

static inline const void AddContainerValue(std::shared_ptr<TRawValueMap> m, std::shared_ptr<TContainer> c, TAbstractValue *v) {
    try {
        auto cv = dynamic_cast<TContainerValue *>(v);
        auto name = cv->GetName();
        cv->SetContainer(c);
        m->Add(name, v);
    } catch (std::bad_cast &e) {
        PORTO_RUNTIME_ERROR(std::string("Invalid variant cast: ") + e.what());
    }
}
// TODO MOVE OUT OF THIS FILE }}}



#define SYNTHESIZE_ACCESSOR(NAME, TYPE) \
    TYPE Get ## NAME(const std::string &name) { \
        return (*Values)[name]->Get<TYPE>(); \
    } \
    TError Set ## NAME(const std::string &name, const TYPE &value) { \
        TError error = (*Values)[name]->Set<TYPE>(value); \
        if (error) \
            return error; \
        TContainerValue *p = GetContainerValue(name); \
        if (p->GetFlags() & PERSISTENT_VALUE) \
            error = Storage->Append(Id, name, (*Values)[name]->ToString()); \
        return error; \
    }

class TVariantSet : public TNonCopyable {
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TRawValueMap> Values;
    std::weak_ptr<TContainer> Container;
    const std::string Id;
    bool Persist;

public:
    TVariantSet(std::shared_ptr<TKeyValueStorage> storage,
                std::shared_ptr<TRawValueMap> values,
                const std::string &id,
                bool persist);
    ~TVariantSet();

    TError Create();
    TError Restore(const kv::TNode &node);

    std::string GetString(const std::string &name) {
        return (*Values)[name]->ToString();
    }
    TError SetString(const std::string &name, const std::string &value) {
        TError error = (*Values)[name]->FromString(value);
        if (error)
            return error;

        TContainerValue *p = GetContainerValue(name);
        if (p->GetFlags() & PERSISTENT_VALUE)
            error = Storage->Append(Id, name, value);
        return error;
    }

    SYNTHESIZE_ACCESSOR(Bool, bool)
    SYNTHESIZE_ACCESSOR(Int, int)
    SYNTHESIZE_ACCESSOR(Uint, uint64_t)
    SYNTHESIZE_ACCESSOR(List, TStrList)
    SYNTHESIZE_ACCESSOR(Map, TUintMap)

    std::vector<std::string> List();
    bool IsDefault(const std::string &name);
    bool HasValue(const std::string &name);
    void Reset(const std::string &name);
    bool IsValid(const std::string &name) { return Values->IsValid(name) == TError::Success(); }
    // TODO MOVE OUT
    TContainerValue *GetContainerValue(const std::string &name);

    TError Flush();
    TError Sync();
};

#undef SYNTHESIZE_ACCESSOR

