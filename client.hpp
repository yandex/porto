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
    int Fd;
    pid_t Pid;
    TCred Cred;
    std::string Comm;
    size_t RequestStartMs;

    TClient(int fd) : Fd(fd) {};
    ~TClient() {
        close(Fd);
    }
};
