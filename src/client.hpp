#pragma once

#include <string>
#include <mutex>
#include <list>

#include "container.hpp"
#include "waiter.hpp"
#include "common.hpp"
#include "epoll.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"

#include "fmt/ostream.h"

class TEpollLoop;

namespace rpc {
    class TContainerRequest;
}

class TRequest;

class TClient : public std::enable_shared_from_this<TClient>,
                public TEpollSource {
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
    bool Processing = false;
    bool Sending = false;
    bool Receiving = false;
    bool WaitRequest = false;
    bool InEpoll = false;
    bool CloseAfterResponse = false;

    TClient(int fd);
    TClient(const std::string &special);
    ~TClient();

    std::unique_lock<std::mutex> Lock() {
        return std::unique_lock<std::mutex>(Mutex);
    }

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

    void CloseConnectionLocked(bool serverShutdown = false);
    void CloseConnection(bool serverShutdown = false) {
        auto lock = Lock();
        CloseConnectionLocked(serverShutdown);
    }

    void StartRequest();
    void FinishRequest();

    TError IdentifyClient(bool initial);
    std::string RelativeName(const std::string &name) const;
    TError ComposeName(const std::string &name, std::string &relative_name) const;
    TError ResolveName(const std::string &relative_name, std::string &name) const;

    TError ResolveContainer(const std::string &relative_name,
                            std::shared_ptr<TContainer> &ct) const;

    TError ReadContainer(const std::string &relative_name,
                         std::shared_ptr<TContainer> &ct);
    TError WriteContainer(const std::string &relative_name,
                          std::shared_ptr<TContainer> &ct, bool child = false);

    TError LockContainer(std::shared_ptr<TContainer> &ct);
    void ReleaseContainer(bool locked = false);

    TPath ComposePath(const TPath &path);
    TPath ResolvePath(const TPath &path);

    TError ControlVolume(const TPath &path, std::shared_ptr<TVolume> &volume,
                         bool read_only = false);

    std::shared_ptr<TContainerWaiter> SyncWaiter;
    std::shared_ptr<TContainerWaiter> AsyncWaiter;
    std::list<TContainerReport> ReportQueue;

    TError Event(uint32_t events);
    TError ReadRequest(rpc::TContainerRequest &request);
    void QueueRequest();
    TError SendResponse(bool first);
    TError QueueResponse(rpc::TContainerResponse &response);
    TError QueueReport(const TContainerReport &report, bool async);
    TError MakeReport(const std::string &name, const std::string &state, bool async,
                      const std::string &label = "", const std::string &value = "");

    std::list<std::weak_ptr<TContainer>> WeakContainers;

private:
    std::mutex Mutex;
    uint64_t ConnectionTime = 0;

    uint64_t Length = 0;
    uint64_t Offset = 0;
    std::vector<uint8_t> Buffer;
    std::unique_ptr<TRequest> Request;
};

extern TClient SystemClient;
extern TClient WatchdogClient;
extern __thread TClient *CL;
