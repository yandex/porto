#pragma once

#include "util/path.hpp"

#include <mutex>

class MeasuredMutex: public std::mutex {
    const std::string Name;

public:
    MeasuredMutex(const std::string &name);

    void lock();
    std::unique_lock<std::mutex> UniqueLock();
};


class TFileMutex {
    TFile File;

public:
    TFileMutex() = delete;
    TFileMutex(const TPath &path, int flags = 0);
    ~TFileMutex();
};
