#ifndef __IDMAP_H__
#define __IDMAP_H__

#include <cstdint>

#include "common.hpp"

constexpr size_t BITS_PER_LLONG = sizeof(unsigned long long) * 8;

class TIdMap : public TNonCopyable {
    unsigned long long Ids[USHRT_MAX / BITS_PER_LLONG];

public:
    TIdMap();
    TError Get(uint16_t &id);
    void Put(uint16_t id);
};

#endif
