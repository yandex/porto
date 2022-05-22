#pragma once

#include <atomic>

#include "error.hpp"

struct TTask {
    pid_t Pid = 0;
    int Status = 0;
    bool Running = false;

    TError Fork(bool detach = false);
    TError Wait(bool interruptible = false,
                const std::atomic_bool &stop = false,
                const std::atomic_bool &disconnected = false);
    static bool Deliver(pid_t pid, int code, int status);

    bool Exists() const;
    bool IsZombie() const;
    pid_t GetPPid() const;
    TError Kill(int signal) const;
    TError KillPg(int signal) const;
};

void LocalTime(const time_t *time, struct tm &tm);
void TaintPostFork(std::string message);
TError TranslatePid(pid_t pid, pid_t pidns, pid_t &result);
