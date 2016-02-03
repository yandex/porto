#pragma once

#include <string>
#include <mutex>
#include <list>

#include "common.hpp"
#include "epoll.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

class TContainer;
class TContainerHolder;
class TContainerWaiter;
class TEpollLoop;

namespace rpc {
    class TContainerRequest;
}

class TClient : public TEpollSource {
public:
    TClient(std::shared_ptr<TEpollLoop> loop, int fd);
    ~TClient();

    void CloseConnection();

    int GetFd() const;
    pid_t GetPid() const;
    const TCred& GetCred() const;
    const std::string& GetComm() const;

    void BeginRequest();
    uint64_t GetRequestTimeMs();

    TError Identify(TContainerHolder &holder, bool full = true);
    std::string GetContainerName() const;
    TError GetContainer(std::shared_ptr<TContainer> &container) const;

    friend std::ostream& operator<<(std::ostream& stream, TClient& client);

    std::shared_ptr<TContainerWaiter> Waiter;
    bool Readonly();

    TError ReadRequest(rpc::TContainerRequest &request);
    bool ReadInterrupted();

    TError QueueResponse(rpc::TContainerResponse &response);
    TError SendResponse(bool first);

    std::list<std::weak_ptr<TContainer>> WeakContainers;

private:
    std::mutex Mutex;
    pid_t Pid;
    TCred Cred;
    std::string Comm;
    uint64_t ConnectionTime;
    uint64_t RequestStartMs;

    TError LoadGroups();
    TError IdentifyContainer(TContainerHolder &holder);
    std::weak_ptr<TContainer> Container;

    bool FullLog = true;

    uint64_t Length = 0;
    uint64_t Offset = 0;
    std::vector<uint8_t> Buffer;
};
