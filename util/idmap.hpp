#ifndef __IDMAP_H__
#define __IDMAP_H__

#include <porto.hpp>
#include "error.hpp"

constexpr size_t BITS_PER_LLONG = sizeof(unsigned long long) * 8;

class TIdMap {
    NO_COPY_CONSTRUCT(TIdMap);
    unsigned long long Ids[UINT16_MAX / BITS_PER_LLONG];

public:
    TIdMap();
    TError Get(uint16_t &id);
    void Put(uint16_t id);
};

#endif
