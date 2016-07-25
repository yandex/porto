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
class TContext;

namespace rpc {
    class TContainerRequest;
}

class TClient : public TEpollSource {
public:
    TCred Cred;
    TCred TaskCred;
    TClient(std::shared_ptr<TEpollLoop> loop);
    TClient(TCred cred);
    ~TClient();

    bool ReadOnlyAccess;

    TError AcceptConnection(TContext &context, int listenFd);
    void CloseConnection();

    int GetFd() const;
    pid_t GetPid() const;

    const std::string& GetComm() const;

    void BeginRequest();
    uint64_t GetRequestTimeMs();

    TError IdentifyClient(TContainerHolder &holder, bool initial);
    TError GetClientContainer(std::shared_ptr<TContainer> &container) const;

    std::string GetContainerName() const;

    TError ComposeRelativeName(const TContainer &target,
                               std::string &relative_name) const;

    TError ResolveRelativeName(const std::string &relative_name,
                               std::string &absolute_name,
                               bool resolve_meta = false) const;

    friend std::ostream& operator<<(std::ostream& stream, TClient& client);

    std::shared_ptr<TContainerWaiter> Waiter;

    TError ReadRequest(rpc::TContainerRequest &request);
    bool ReadInterrupted();

    TError QueueResponse(rpc::TContainerResponse &response);
    TError SendResponse(bool first);

    std::list<std::weak_ptr<TContainer>> WeakContainers;

private:
    std::mutex Mutex;
    pid_t Pid;
    std::string Comm;
    uint64_t ConnectionTime;
    uint64_t RequestStartMs;
    bool Processing = false;

    TError LoadGroups();

    bool FullLog = true;

    uint64_t Length = 0;
    uint64_t Offset = 0;
    std::vector<uint8_t> Buffer;
    std::weak_ptr<TContainer> ClientContainer;
};
