#pragma once

#include <vector>
#include <algorithm>

#include "common.hpp"
#include "log.hpp"

class TIdMap : public TNonCopyable {
private:
    std::vector<bool> Used;
public:
    TIdMap(int size) {
        Used.resize(size, false);
    }

    TError GetAt(int id) {
        if (id < 1 || id > (int)Used.size())
            return TError(EError::Unknown, "Id " + std::to_string(id) + " out of range");
        if (Used[id - 1])
            return TError(EError::Unknown, "Id " + std::to_string(id) + " already used");
        Used[id - 1] = true;
        return TError::Success();
    }

    TError Get(int &id) {
        auto it = std::find(Used.begin(), Used.end(), false);
        if (it == Used.end())
            TError(EError::ResourceNotAvailable, "Cannot allocate id");
        id = it - Used.begin() + 1;
        return GetAt(id);
    }

    void Put(int id) {
        PORTO_ASSERT(id > 0 && id <= (int)Used.size());
        PORTO_ASSERT(Used[id - 1]);
        Used[id - 1] = false;
    }
};
