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
    std::string Name;
    std::map<std::string, std::string> Data;

    TKeyValue(const TPath &path) : Path(path) { }

    friend bool operator<(const TKeyValue &lhs, const TKeyValue &rhs) {
        return lhs.Name < rhs.Name;
    }

    bool Has(const std::string &key) const {
        return Data.count(key);
    }

    std::string Get(const std::string &key) const {
        auto it = Data.find(key);
        return it == Data.end() ? "" : it->second;
    }

    void Set(const std::string &key, const std::string &val) {
        Data[key] = val;
    }

    void Del(const std::string &key) {
        Data.erase(key);
    }

    TError Load();
    TError Save();

    static TError Mount(const TPath &root);
    static TError ListAll(const TPath &root, std::list<TKeyValue> &nodes);
    static void DumpAll(const TPath &root);
};
