#pragma once

#include <vector>
#include <algorithm>

#include "common.hpp"
#include "log.hpp"

class TIdMap : public TNonCopyable {
private:
    int Base;
    int Last = -1;
    std::vector<bool> Used;
public:
    TIdMap(int base, int size) {
        Base = base;
        Resize(size);
    }

    void Resize(int size) {
        Used.resize(size, false);
    }

    TError GetAt(int id) {
        if (id < Base || id >= Base + (int)Used.size())
            return TError("Id " + std::to_string(id) + " out of range");
        if (Used[id - Base])
            return TError("Id " + std::to_string(id) + " already used");
        Used[id - Base] = true;
        return OK;
    }

    TError Get(int &id) {
        auto it = std::find(Used.begin() + Last + 1, Used.end(), false);
        if (Last && it == Used.end())
            it = std::find(Used.begin(), Used.end(), false);
        if (it == Used.end()) {
            id = -1;
            return TError(EError::ResourceNotAvailable, "Cannot allocate id");
        }
        Last = it - Used.begin();
        id = Base + Last;
        *it = true;
        return OK;
    }

    TError Put(int id) {
        if (id < Base || id >= Base + (int)Used.size())
            return TError("Id out of range");
        if (!Used[id - Base])
            return TError("Freeing not allocated id");
        Used[id - Base] = false;
        return OK;
    }
};
