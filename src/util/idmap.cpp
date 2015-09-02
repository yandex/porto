#include <climits>

#include "idmap.hpp"

TIdMap::TIdMap() {
    for (auto &i : Ids)
        i = ULLONG_MAX;
}

TError TIdMap::Get(uint16_t &id) {
    return GetSince(0, id);
}

TError TIdMap::GetSince(uint16_t since, uint16_t &id) {
    size_t start = since / BITS_PER_LLONG;
    if (since % BITS_PER_LLONG)
        start++;

    for (size_t i = start; i < sizeof(Ids) / sizeof(Ids[0]); i++) {
        int bit = ffsll(Ids[i]);
        if (bit == 0)
            continue;

        id = i * BITS_PER_LLONG + bit;

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

    if ((Ids[bucket] & (1ULL << bit)) == 0)
        return TError(EError::Unknown, "Id " + std::to_string(id + 1) + " already used");

    Ids[bucket] &= ~(1ULL << bit);
    return TError::Success();
}

void TIdMap::Put(uint16_t id) {
    id--;

    int bucket = id / BITS_PER_LLONG;
    int bit = id % BITS_PER_LLONG;

    Ids[bucket] |= 1ULL << bit;
}
