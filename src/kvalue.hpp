#pragma once

#include <string>
#include <map>
#include <list>
#include "common.hpp"
#include "util/path.hpp"

class TKeyValue {
public:
    TPath Path;
    int Id = 0;
    TString Name;
    std::map<TString, TString> Data;

    TKeyValue(const TPath &path) : Path(path) { }

    friend bool operator<(const TKeyValue &lhs, const TKeyValue &rhs) {
        return lhs.Name < rhs.Name;
    }

    bool Has(const TString &key) const {
        return Data.count(key);
    }

    TString Get(const TString &key) const {
        auto it = Data.find(key);
        return it == Data.end() ? "" : it->second;
    }

    void Set(const TString &key, const TString &val) {
        Data[key] = val;
    }

    void Del(const TString &key) {
        Data.erase(key);
    }

    TError Load();
    TError Save();

    static TError Mount(const TPath &root);
    static TError ListAll(const TPath &root, std::list<TKeyValue> &nodes);
    static void DumpAll(const TPath &root);
};
