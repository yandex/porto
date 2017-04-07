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
    bool InEpoll = false;

    TClient(int fd);
    TClient(const std::string &special);
    ~TClient();

    EAccessLevel AccessLevel = EAccessLevel::None;
    std::string PortoNamespace;
    std::string WriteNamespace;

    bool IsSuperUser(void) const;

    bool CanSetUidGid() const;
    TError CanControl(const TCred &cred);
    TError CanControl(const TContainer &ct, bool child = false);

    TError ReadAccess(const TFile &file);
    TError WriteAccess(const TFile &file);

    void CloseConnection();

    void StartRequest();
    void FinishRequest();

    TError IdentifyClient(bool initial);
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
    TPath DefaultPlace();
    TError CanControlPlace(const TPath &place);

    template<typename ostream>
    friend ostream& operator<<(ostream& stream, const TClient &client) {
        stream << client.Fd << ":" << client.Comm << "(" << client.Pid << ")";

        if (client.FirstLog) {
            const_cast<TClient &>(client).FirstLog = false;
            stream << " " << client.TaskCred;
            if (client.Cred.Uid != client.TaskCred.Uid ||
                    client.Cred.Gid != client.TaskCred.Gid)
                stream << " owner " << client.Cred;
            if (client.ClientContainer) {
                stream << " from " << client.ClientContainer->Name;
                if (client.PortoNamespace != "")
                    stream << " namespace " << client.PortoNamespace;
                if (client.WriteNamespace != client.PortoNamespace)
                    stream << " write-namespace " << client.WriteNamespace;
            }
        }

        return stream;
    }

    std::shared_ptr<TContainerWaiter> Waiter;

    TError ReadRequest(rpc::TContainerRequest &request);
    bool ReadInterrupted();

    TError QueueResponse(rpc::TContainerResponse &response);
    TError SendResponse(bool first);

    std::list<std::weak_ptr<TContainer>> WeakContainers;

private:
    std::mutex Mutex;
    uint64_t ConnectionTime = 0;

    bool FirstLog = true;

    uint64_t Length = 0;
    uint64_t Offset = 0;
    std::vector<uint8_t> Buffer;
};

extern TClient SystemClient;
extern __thread TClient *CL;
