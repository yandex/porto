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

    bool HasValue() const;
    void Reset();
    int GetFlags() const;

    template<typename T>
    const T Get() const {
        try {
            auto p = dynamic_cast<const TValue<T> *>(this);
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
    TAbstractValue *Find(const std::string &name) const;
    bool IsValid(const std::string &name) const;
    bool IsDefault(const std::string &name) const;
    bool HasValue(const std::string &name) const;
    std::vector<std::string> List() const;
};

class TValueMap : public TRawValueMap, public TNonCopyable {
    std::shared_ptr<TKeyValueNode> KvNode;
    TError Flush();
    TError Sync();

public:
    TValueMap(std::shared_ptr<TKeyValueNode> kvnode) : KvNode(kvnode) {}
    ~TValueMap();

    TError Create();
    TError Restore(const kv::TNode &node);
    TError Restore();

    std::string ToString(const std::string &name) const;
    TError FromString(const std::string &name, const std::string &value);

    template<typename T>
    const T Get(const std::string &name) const {
        return Find(name)->Get<T>();
    }

    template<typename T>
    bool IsDefaultValue(const std::string &name, const T& value) {
        try {
            auto av = Find(name);
            auto p = dynamic_cast<TValue<T> *>(av);
            return p->GetDefault() == value;
        } catch (std::bad_cast &e) {
            PORTO_RUNTIME_ERROR(std::string("Bad cast: ") + e.what());
        }
        return false;
    }

    template<typename T>
    TError Set(const std::string &name, const T& value) {
        bool resetOnDefault = IsDefaultValue(name, value);
        TError error = Find(name)->Set<T>(value);
        if (error)
            return error;

        if (KvNode && Find(name)->GetFlags() & PERSISTENT_VALUE)
            error = KvNode->Append(name, Find(name)->ToString());

        // we don't want to keep default values in memory but we also
        // want custom TValue descendants to do some internal preparation
        // even if we set value to default; so just set it and reset
        // afterwards
        if (resetOnDefault)
            Find(name)->Reset();

        return error;
    }

    void Reset(const std::string &name);
};
