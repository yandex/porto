#pragma once

#include <string>

#include "common.hpp"
#include "util/cred.hpp"
#include "util/log.hpp"
#include "container.hpp"

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

    TError Identify(TContainerHolder &holder, bool full = true);
    std::string GetContainerName() const;
    std::shared_ptr<TContainer> GetContainer() const;

private:
    int Fd;
    pid_t Pid;
    TCred Cred;
    std::string Comm;
    size_t RequestStartMs;

    TError IdentifyContainer(TContainerHolder &holder);
    std::weak_ptr<TContainer> Container;
};
