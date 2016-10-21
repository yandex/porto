#pragma once

#include <string>
#include <mutex>
#include <list>

#include "common.hpp"
#include "epoll.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

class TContainer;
class TContainerWaiter;
class TEpollLoop;

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
    std::shared_ptr<TContainer> ClientContainer;
    std::shared_ptr<TContainer> LockedContainer;

    TClient();
    TClient(const std::string &special);
    ~TClient();

    EAccessLevel AccessLevel = EAccessLevel::None;

    bool IsSuperUser(void) const;

    bool CanSetUidGid() const;
    TError CanControl(const TCred &cred);
    TError CanControl(const TContainer &ct, bool child = false);

    TError AcceptConnection(int listenFd);
    void CloseConnection();

    void StartRequest();
    void FinishRequest();

    uint64_t GetRequestTimeMs();

    TError IdentifyClient(bool initial);
    TError ComposeName(const std::string &name, std::string &relative_name) const;
    TError ResolveName(const std::string &relative_name, std::string &name) const;

    TError ResolveContainer(const std::string &relative_name,
                            std::shared_ptr<TContainer> &ct) const;
    TError ReadContainer(const std::string &relative_name,
                         std::shared_ptr<TContainer> &ct, bool try_lock = false);
    TError WriteContainer(const std::string &relative_name,
                          std::shared_ptr<TContainer> &ct, bool child = false);
    void ReleaseContainer(bool locked = false);

    TPath ComposePath(const TPath &path);
    TPath ResolvePath(const TPath &path);

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
};

extern TClient SystemClient;
extern __thread TClient *CurrentClient;
