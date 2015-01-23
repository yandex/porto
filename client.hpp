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

    TClient(int fd) : Fd(fd) {};
    TClient() {
        L() << "pid " << Pid
            << " uid " << Cred.Uid
            << " gid " << Cred.Gid
            << " disconnected" << std::endl;

        close(Fd);
    }
};
