#pragma once

#include <string>
#include <map>
#include <memory>
#include <set>

#include "common.hpp"
#include "kvalue.hpp"
#include "util/log.hpp"

// Property can be modified only by privileged user
const unsigned int SUPERUSER_PROPERTY = (1 << 0);
// Property should return parent value as default
const unsigned int PARENT_DEF_PROPERTY = (1 << 1);
// Property can be modified only by restricted root
const unsigned int RESTROOT_PROPERTY = (1 << 3);
// Properties marked with this flag are reverted to default upon container
// start with virt_mode==os
const unsigned int OS_MODE_PROPERTY = (1 << 4);

// Lack of support in kernel
const unsigned int UNSUPPORTED_FEATURE = (1 << 25);
// Value has not saved
const unsigned int DIRTY_VALUE = (1 << 26);
// Value has non-default value
const unsigned int HAS_VALUE = (1 << 27);
// User cannot modify value
const unsigned int READ_ONLY_VALUE = (1 << 28);
// Value should be preserved upon recovery
const unsigned int PERSISTENT_VALUE = (1 << 30);
// Value is not shown in the property/data list
const unsigned int HIDDEN_VALUE = (1 << 31);

class TValue : public TNonCopyable {
protected:
    int Flags;
public:
    TValue(int flags) : Flags(flags) {}
    virtual ~TValue() {}

    bool HasFlag(int flag) const { return (Flags & flag) != 0; }
    void SetFlag(int flag) { Flags |= flag; }
    void ClearFlag(int flag) { Flags &= ~flag; }

    bool HasValue() const { return HasFlag(HAS_VALUE); }

    virtual void Reset() = 0;

    virtual TError GetString(std::string &value) const =0;
    virtual TError SetString(const std::string &value) =0;

    virtual TError GetIndexed(const std::string &index, std::string &value) const =0;
    virtual TError SetIndexed(const std::string &index, const std::string &value) =0;
};

class TTextValue : public TValue {
public:
    TTextValue(int flags) : TValue(flags) {}

    void Reset() override {  }

    TError SetString(const std::string &value) override {
        return TError(EError::InvalidValue, "This is read-only value");
    }

    TError SetIndexed(const std::string &index, const std::string &value) override {
        return TError(EError::InvalidValue, "This is read-only value");
    }
};

template<typename T>
class TStoredValue : public TValue {
private:
    T Value;
public:
    TStoredValue(int flags) : TValue(flags) {}

    virtual T GetDefault() const =0;
    virtual std::string ToString(const T &value) const =0;
    virtual TError FromString(const std::string &value, T &result) const =0;

    virtual TError GetString(std::string &value) const override {
        value = ToString(Get());
        return TError::Success();
    }

    virtual TError SetString(const std::string &value) override {
        T result;
        TError error = FromString(value, result);
        if (!error)
            error = Set(result);
        return error;
    }

    TError GetIndexed(const std::string &index, std::string &value) const override {
        return TError(EError::InvalidValue, "Invalid subscript for property");
    }

    TError SetIndexed(const std::string &index, const std::string &value) override {
        return TError(EError::InvalidValue, "Invalid subscript for property");
    }

    virtual TError CheckValue(const T& value) {
        return TError::Success();
    }

    virtual const T Get() const {
        if (HasValue())
            return Value;
        return GetDefault();
    }

    virtual TError Set(const T &value) {
        TError error = CheckValue(value);
        if (error)
            return error;
        if (HasFlag(PERSISTENT_VALUE) && (!HasValue() || Value != value))
            SetFlag(DIRTY_VALUE);
        Value = value;
        SetFlag(HAS_VALUE);
        return TError::Success();
    }

    virtual void Reset() override {
        if (HasFlag(PERSISTENT_VALUE) && HasValue())
            SetFlag(DIRTY_VALUE);
        Value = GetDefault();
        ClearFlag(HAS_VALUE);
    }
};

class TStringValue : public TStoredValue<std::string> {
public:
    TStringValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const std::string &value) const override;
    TError FromString(const std::string &value, std::string &result) const override;
    std::string GetDefault() const override;
};

class TIntValue : public TStoredValue<int> {
public:
    TIntValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const int &value) const override;
    TError FromString(const std::string &value, int &result) const override;
    int GetDefault() const override;
};

class TUintValue : public TStoredValue<uint64_t> {
public:
    TUintValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const uint64_t &value) const override;
    TError FromString(const std::string &value, uint64_t &result) const override;
    uint64_t GetDefault() const override;
};

class TSizeValue : public TStoredValue<uint64_t> {
public:
    TSizeValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const uint64_t &value) const override;
    TError FromString(const std::string &value, uint64_t &result) const override;
    uint64_t GetDefault() const override;
};

class TDoubleValue : public TStoredValue<double> {
public:
    TDoubleValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const double &value) const override;
    TError FromString(const std::string &value, double &result) const override;
    double GetDefault() const override;
};

class TBoolValue : public TStoredValue<bool> {
public:
    TBoolValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const bool &value) const override;
    TError FromString(const std::string &value, bool &result) const override;
    bool GetDefault() const override;
};

class TIntListValue : public TStoredValue<std::vector<int>> {
public:
    TIntListValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const std::vector<int> &value) const override;
    TError FromString(const std::string &value, std::vector<int> &result) const override;
    std::vector<int> GetDefault() const override;
};

typedef std::vector<std::string> TStrList;

class TListValue : public TStoredValue<TStrList> {
public:
    TListValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const TStrList &value) const override;
    TError FromString(const std::string &value, TStrList &result) const override;
    TStrList GetDefault() const override;
};

typedef std::map<std::string, uint64_t> TUintMap;

class TMapValue : public TStoredValue<TUintMap> {
public:
    TMapValue(int flags) : TStoredValue(flags) {}

    std::string ToString(const TUintMap &value) const override;
    TError FromString(const std::string &value, TUintMap &result) const override;
    TUintMap GetDefault() const override;

    TError GetIndexed(const std::string &index, std::string &value) const override;
    TError SetIndexed(const std::string &index, const std::string &value) override;
};

class TRawValueMap {
protected:
    std::map<std::string, TValue *> Values;

public:
    virtual ~TRawValueMap();

    TError Add(const std::string &name, TValue *av);
    TValue *Find(const std::string &name) const;
    bool IsDefault(const std::string &name) const;
    bool HasValue(const std::string &name) const;
    bool IsReadOnly(const std::string &name) const;
    std::vector<std::string> List() const;
};

class TValueMap : public TRawValueMap, public TNonCopyable {
    std::shared_ptr<TKeyValueNode> KvNode;

public:
    TValueMap(std::shared_ptr<TKeyValueNode> kvnode) : KvNode(kvnode) {}

    TError Create();
    TError Remove();
    TError Restore(const kv::TNode &node);
    TError Restore();
    TError Flush();
    TError Sync();

    TError SetValue(const std::string &name, const std::string &value);

    template<typename T>
    const T Get(const std::string &name) const {
        try {
            auto p = dynamic_cast<const TStoredValue<T> *>(Find(name));
            if (!p)
                PORTO_RUNTIME_ERROR(std::string("Bad cast"));
            return p->Get();
        } catch (std::bad_cast &e) {
            PORTO_RUNTIME_ERROR(std::string("Bad cast: ") + e.what());
            return {};
        }
    }

    template<typename T>
    TError Set(const std::string &name, const T& value) {
        try {
            auto p = dynamic_cast<TStoredValue<T> *>(Find(name));
            if (!p)
                PORTO_RUNTIME_ERROR(std::string("Bad cast"));
            TError error = p->Set(value);
            if (!error && KvNode && p->HasFlag(PERSISTENT_VALUE))
                error = KvNode->Append(name, p->ToString(value));
            return error;
        } catch (std::bad_cast &e) {
            PORTO_RUNTIME_ERROR(std::string("Bad cast: ") + e.what());
            return TError(EError::Unknown, "");
        }
    }
};
