#pragma once

#include <atomic>
#include <sstream>
#include <vector>

#include "util/error.hpp"

class THistogram {
    const std::vector<unsigned> Buckets;
    std::vector<std::atomic<uint64_t>> Values;

public:
    THistogram(const std::vector<unsigned> &buckets)
    : Buckets(buckets)
    , Values(buckets.size())
    {}

    inline int GetBucket(unsigned value) const {
        return (std::upper_bound(Buckets.begin(), Buckets.end(), value) - Buckets.begin()) - 1;
    }

    void Add(unsigned value) {
        int pos = GetBucket(value);
        if (pos != -1)
            ++Values[pos];
    }

    std::string Format() const {
        std::stringstream ss;

        for (size_t i = 0; i < Buckets.size(); ++i)
            ss << (i ? ";" : "") << Buckets[i] << ":" << Values[i];

        return ss.str();
    }
};
