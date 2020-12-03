#pragma once

#include <mutex>

class MeasuredMutex: public std::mutex {
    const std::string Name;

public:
    MeasuredMutex(const std::string &name);

    void lock();
    std::unique_lock<std::mutex> UniqueLock();
};
