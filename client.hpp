#pragma once

#include <map>
#include "common.hpp"
#include "util/cred.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
};

class TClient : public TNonCopyable {
public:
    TClient(int fd);
    ~TClient();

    int GetFd() const;
    pid_t GetPid() const;
    const TCred& GetCred() const;
    const std::string& GetComm() const;

    size_t GetRequestStartMs() const;
    void SetRequestStartMs(size_t start);

    TError Identify(bool full = true);

private:
    int Fd;
    pid_t Pid;
    TCred Cred;
    std::string Comm;
    size_t RequestStartMs;
};
