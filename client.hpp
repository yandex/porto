#pragma once

#include <string>
#include <mutex>

#include "common.hpp"
#include "util/cred.hpp"
#include "util/log.hpp"
#include "container.hpp"

extern "C" {
#include <unistd.h>
};

class TContainerWaiter;

class TClient : public TNonCopyable {
public:
    std::mutex Lock;

    TClient(int fd);
    ~TClient();

    int GetFd() const;
    pid_t GetPid() const;
    const TCred& GetCred() const;
    const std::string& GetComm() const;

    void BeginRequest();
    size_t GetRequestTime();

    TError Identify(TContainerHolder &holder, bool full = true);
    std::string GetContainerName() const;
    std::shared_ptr<TContainer> GetContainer() const;

    friend std::ostream& operator<<(std::ostream& stream, TClient& client) {
        if (client.FullLog) {
            client.FullLog = false;
            stream << client.Comm << "(" << client.Pid << ") "
                   << client.Cred.UserAsString() << ":"
                   << client.Cred.GroupAsString() << " "
                   << client.GetContainerName();
        } else {
            stream << client.Comm << "(" << client.Pid << ")";
        }
        return stream;
    }

    std::shared_ptr<TContainerWaiter> Waiter;

private:
    int Fd;
    pid_t Pid;
    TCred Cred;
    std::string Comm;
    size_t RequestStartMs;

    TError IdentifyContainer(TContainerHolder &holder);
    std::weak_ptr<TContainer> Container;

    bool FullLog = true;
};
