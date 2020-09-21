#pragma once

#include <functional>
#include <thread>
#include <unordered_set>

#include "unix.hpp"

extern std::mutex TidsMutex;
extern std::unordered_set<pid_t> PortoTids;

template <class F, class... Args>
void ThreadWrapper(F&& f, Args&&... args) {
    pid_t tid = GetTid();

    std::unique_lock<std::mutex> lock(TidsMutex);
    PortoTids.insert(tid);
    lock.unlock();

    std::ref(f)(std::forward<Args>(args)...);
}

// Attention, Args are taken by values
template <class F, class... Args>
std::thread *NewThread(F&& f, Args... args) {
    return new std::thread(&ThreadWrapper<F, Args...>, std::forward<F>(f), std::forward<Args>(args)...);
}
