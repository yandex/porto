#pragma once

#include <string>
#include <map>
#include <memory>
#include <set>

#include "common.hpp"
#include "kvalue.hpp"
#include "util/log.hpp"

// Value is not shown in the property/data list
const unsigned int HIDDEN_VALUE = (1 << 31);
// Value should be preserved upon recovery
const unsigned int PERSISTENT_VALUE = (1 << 30);
// Uint value can include options G/M/K suffix
const unsigned int UINT_UNIT_VALUE = (1 << 29);

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
    void Set(const T &value) {
        Impl = std::make_shared<TValueImpl<T>>(value);
    };

    void Reset() { Impl = nullptr; }
};

template<typename T>
class TValue;

class TAbstractValue : public TNonCopyable {
protected:
    TVariant Variant;
    int Flags;
public:
    TAbstractValue(int flags) : Flags(flags) {}
    virtual ~TAbstractValue() {}

    virtual std::string ToString() const =0;
    virtual TError FromString(const std::string &value) =0;
    virtual std::string DefaultString() const =0;

    bool HasValue() { return Variant.HasValue(); }
    void Reset() { Variant.Reset(); }
    int GetFlags() { return Flags; }

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
            return TError(EError::Unknown, "");
        }
    }
};

template<typename T>
class TValue : public TAbstractValue {
public:
    TValue(int flags) : TAbstractValue(flags) {}

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
        if (Variant.HasValue())
            return Variant.Get<T>();
        else
            return GetDefault();
    }

    TError Set(const T &value) {
        TError error = CheckValue(value);
        if (error)
            return error;

        Variant.Set(value);

        return TError::Success();
    }
};

class TStringValue : public TValue<std::string> {
public:
    TStringValue(int flags) : TValue(flags) {}

    std::string ToString(const std::string &value) const override;
    TError FromString(const std::string &value) override;
    std::string GetDefault() const override;
};

class TIntValue : public TValue<int> {
public:
    TIntValue(int flags) : TValue(flags) {}

    std::string ToString(const int &value) const override;
    TError FromString(const std::string &value) override;
    int GetDefault() const override;
};

class TUintValue : public TValue<uint64_t> {
public:
    TUintValue(int flags) : TValue(flags) {}

    std::string ToString(const uint64_t &value) const override;
    TError FromString(const std::string &value) override;
    uint64_t GetDefault() const override;
};

class TBoolValue : public TValue<bool> {
public:
    TBoolValue(int flags) : TValue(flags) {}

    std::string ToString(const bool &value) const override;
    TError FromString(const std::string &value) override;
    bool GetDefault() const override;
};

typedef std::vector<std::string> TStrList;

class TListValue : public TValue<TStrList> {
public:
    TListValue(int flags) : TValue(flags) {}

    std::string ToString(const TStrList &value) const override;
    TError FromString(const std::string &value) override;
    TStrList GetDefault() const override;
};

typedef std::map<std::string, uint64_t> TUintMap;

class TMapValue : public TValue<TUintMap> {
public:
    TMapValue(int flags) : TValue(flags) {}

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
    bool HasValue(const std::string &name) const;
    std::vector<std::string> List() const;
};

// TODO: rework into template method
#define SYNTHESIZE_ACCESSOR(NAME, TYPE) \
    TYPE Get ## NAME(const std::string &name) { \
        return (*Values)[name]->Get<TYPE>(); \
    } \
    TError Set ## NAME(const std::string &name, const TYPE &value) { \
        TError error = (*Values)[name]->Set<TYPE>(value); \
        if (error) \
            return error; \
        if ((*Values)[name]->GetFlags() & PERSISTENT_VALUE) \
            error = Storage->Append(Id, name, (*Values)[name]->ToString()); \
        return error; \
    }

class TVariantSet : public TNonCopyable {
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TRawValueMap> Values;
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

        if ((*Values)[name]->GetFlags() & PERSISTENT_VALUE)
            error = Storage->Append(Id, name, value);
        return error;
    }

    TAbstractValue *operator[](const std::string &name) const { return (*Values)[name]; }

    SYNTHESIZE_ACCESSOR(Bool, bool)
    SYNTHESIZE_ACCESSOR(Int, int)
    SYNTHESIZE_ACCESSOR(Uint, uint64_t)
    SYNTHESIZE_ACCESSOR(List, TStrList)
    SYNTHESIZE_ACCESSOR(Map, TUintMap)

    std::vector<std::string> List();
    bool IsDefault(const std::string &name) const;
    bool HasValue(const std::string &name) const;
    void Reset(const std::string &name);
    bool IsValid(const std::string &name) { return Values->IsValid(name) == TError::Success(); }

    TError Flush();
    TError Sync();
};

#undef SYNTHESIZE_ACCESSOR
