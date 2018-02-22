#pragma once

#include <string>
#include <mutex>
#include <list>

#include "container.hpp"
#include "common.hpp"
#include "epoll.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

#include "fmt/ostream.h"

class TEpollLoop;

namespace rpc {
    class TContainerRequest;
}

class TClient : public TEpollSource {
public:
    std::string Id;
    TCred Cred;
    TCred TaskCred;
    pid_t Pid = 0;
    std::string Comm;
    gid_t UserCtGroup = 0;
    std::shared_ptr<TContainer> ClientContainer;
    std::shared_ptr<TContainer> LockedContainer;
    uint64_t ActivityTimeMs = 0;
    uint64_t RequestTimeMs = 0;
    bool Processing = false;
    bool WaitRequest = false;
    bool InEpoll = false;

    TClient(int fd);
    TClient(const std::string &special);
    ~TClient();

    EAccessLevel AccessLevel = EAccessLevel::None;
    std::string PortoNamespace;
    std::string WriteNamespace;

    bool IsSuperUser(void) const;

    bool IsInternalUser(void) const {
        return AccessLevel == EAccessLevel::Internal;
    }

    bool IsBlockShutdown() const {
        return (Processing && !WaitRequest) || Offset;
    }

    bool CanSetUidGid() const;
    TError CanControl(const TCred &cred);
    TError CanControl(const TContainer &ct, bool child = false);

    TError ReadAccess(const TFile &file);
    TError WriteAccess(const TFile &file);

    void CloseConnection();

    void StartRequest();
    void FinishRequest();

    TError IdentifyClient(bool initial);
    std::string RelativeName(const std::string &name) const;
    TError ComposeName(const std::string &name, std::string &relative_name) const;
    TError ResolveName(const std::string &relative_name, std::string &name) const;

    TError ResolveContainer(const std::string &relative_name,
                            std::shared_ptr<TContainer> &ct) const;

    TError ReadContainer(const std::string &relative_name,
                         std::shared_ptr<TContainer> &ct, bool try_lock = false);
    TError WriteContainer(const std::string &relative_name,
                          std::shared_ptr<TContainer> &ct, bool child = false);

    TError LockContainer(std::shared_ptr<TContainer> &ct);
    void ReleaseContainer(bool locked = false);

    TPath ComposePath(const TPath &path);
    TPath ResolvePath(const TPath &path);

    TError ResolveVolume(const TPath &path, std::shared_ptr<TVolume> &volume);

    TError ControlVolume(const TPath &path, std::shared_ptr<TVolume> &volume);

    TPath DefaultPlace();
    TError CanControlPlace(const TPath &place);


    std::shared_ptr<TContainerWaiter> Waiter;

    TError ReadRequest(rpc::TContainerRequest &request);
    bool ReadInterrupted();

    TError QueueResponse(rpc::TContainerResponse &response);
    TError SendResponse(bool first);

    std::list<std::weak_ptr<TContainer>> WeakContainers;

private:
    std::mutex Mutex;
    uint64_t ConnectionTime = 0;

    uint64_t Length = 0;
    uint64_t Offset = 0;
    std::vector<uint8_t> Buffer;
};

extern TClient SystemClient;
extern TClient WatchdogClient;
extern __thread TClient *CL;
