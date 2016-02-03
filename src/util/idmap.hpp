#pragma once

#include <vector>
#include <algorithm>

#include "common.hpp"
#include "log.hpp"

class TIdMap : public TNonCopyable {
private:
    int Base;
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
            return TError(EError::Unknown, "Id " + std::to_string(id) + " out of range");
        if (Used[id - Base])
            return TError(EError::Unknown, "Id " + std::to_string(id) + " already used");
        Used[id - Base] = true;
        return TError::Success();
    }

    TError Get(int &id) {
        auto it = std::find(Used.begin(), Used.end(), false);
        if (it == Used.end()) {
            id = -1;
            return TError(EError::ResourceNotAvailable, "Cannot allocate id");
        }
        id = Base + (it - Used.begin());
        *it = true;
        return TError::Success();
    }

    TError Put(int id) {
        if (id < Base || id >= Base + (int)Used.size())
            return TError(EError::Unknown, "Id out of range");
        if (!Used[id - Base])
            return TError(EError::Unknown, "Freeing not allocated id");
        Used[id - Base] = false;
        return TError::Success();
    }
};
