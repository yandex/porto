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
        Ids[i] &= ~(1 << bit);
        id = i * BITS_PER_LLONG + bit;
        id++;

        return TError::Success();
    }

    return TError(EError::ResourceNotAvailable, "Can't allocate id");
}

void TIdMap::Put(uint16_t id) {
    id--;

    int bucket = id / BITS_PER_LLONG;
    int bit = id % BITS_PER_LLONG;

    Ids[bucket] |= 1 << bit;
}
