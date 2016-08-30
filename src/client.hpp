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
    pid_t Pid = 0;
    std::string Comm;
    gid_t UserCtGroup = 0;

    TClient(std::shared_ptr<TEpollLoop> loop);
    TClient(const std::string &special);
    ~TClient();

    EAccessLevel AccessLevel = EAccessLevel::None;

    bool IsSuperUser(void) const;
    TError CanControl(const TCred &cred);
    TError CanControl(const TContainer &ct, bool createChild = false);

    TError AcceptConnection(TContext &context, int listenFd);
    void CloseConnection();

    void StartRequest();
    void FinishRequest();

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
    uint64_t ConnectionTime = 0;
    uint64_t RequestStartMs = 0;
    bool Processing = false;

    TError LoadGroups();

    bool FullLog = true;

    uint64_t Length = 0;
    uint64_t Offset = 0;
    std::vector<uint8_t> Buffer;
    std::weak_ptr<TContainer> ClientContainer;
};

extern TClient SystemClient;
extern __thread TClient *CurrentClient;
