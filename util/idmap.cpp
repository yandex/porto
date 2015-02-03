#include <climits>

#include "idmap.hpp"

TIdMap::TIdMap() {
    for (auto &i : Ids) {
        i = ULLONG_MAX;
    }
}

TError TIdMap::Get(uint16_t &id) {
    for (size_t i = 0; i < sizeof(Ids) / sizeof(Ids[0]); i++) {
        int bit = ffsll(Ids[i]);
        if (bit == 0)
            continue;

        bit--;

        id = i * BITS_PER_LLONG + bit;
        id++;

        TError error = GetAt(id);
        if (error)
            return error;

        return TError::Success();
    }

    return TError(EError::ResourceNotAvailable, "Can't allocate id");
}

TError TIdMap::GetAt(uint16_t id) {
    id--;

    int bucket = id / BITS_PER_LLONG;
    int bit = id % BITS_PER_LLONG;

    if ((Ids[bucket] & (1 << bit)) == 0)
        return TError(EError::Unknown, "Id " + std::to_string(id) + " already used");

    Ids[bucket] &= ~(1 << bit);
    return TError::Success();
}

void TIdMap::Put(uint16_t id) {
    id--;

    int bucket = id / BITS_PER_LLONG;
    int bit = id % BITS_PER_LLONG;

    Ids[bucket] |= 1 << bit;
}
