#pragma once

#include <cstdint>

#include "common.hpp"

constexpr size_t BITS_PER_LLONG = sizeof(unsigned long long) * 8;
constexpr uint16_t MAX_UINT16 = (uint16_t)-1;

class TIdMap : public TNonCopyable {
    unsigned long long Ids[MAX_UINT16 / BITS_PER_LLONG];

public:
    TIdMap();
    TError Get(uint16_t &id);
    TError GetAt(uint16_t id);
    void Put(uint16_t id);
};
