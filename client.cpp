#include <algorithm>

#include "client.hpp"

#include "util/file.hpp"
#include "holder.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/socket.h>
};

TClient::TClient(int fd) : Fd(fd) {
}

TClient::~TClient() {
    close(Fd);
}

int TClient::GetFd() const {
    return Fd;
}

pid_t TClient::GetPid() const {
    return Pid;
}

const TCred& TClient::GetCred() const {
    return Cred;
}

const std::string& TClient::GetComm() const {
    return Comm;
}

void TClient::BeginRequest() {
    RequestStartMs = GetCurrentTimeMs();
}

size_t TClient::GetRequestTime() {
    return GetCurrentTimeMs() - RequestStartMs;
}

TError TClient::Identify(TContainerHolder &holder, bool full) {
    struct ucred cr;
    socklen_t len = sizeof(cr);

    if (getsockopt(Fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0) {
        if (full) {
            TFile f("/proc/" + std::to_string(cr.pid) + "/comm");
            std::string comm;

            if (f.AsString(comm))
                comm = "unknown process";

            comm.erase(std::remove(comm.begin(), comm.end(), '\n'), comm.end());

            Pid = cr.pid;
            Comm = comm;

            TError err = IdentifyContainer(holder);
            if (err) {
                L_WRN() << "Can't identify container of pid " << cr.pid
                        << " : " << err << std::endl;
                return err;
            }
        } else {
            if (Container.expired())
                return TError(EError::Unknown, "Can't identify client (container is dead)");
        }

        Cred.Uid = cr.uid;
        Cred.Gid = cr.gid;

        return TError::Success();
    } else {
        return TError(EError::Unknown, "Can't identify client (getsockopt() failed)");
    }
}

TError TClient::IdentifyContainer(TContainerHolder &holder) {
    std::map<std::string, std::string> cgmap;
    TError error = GetTaskCgroups(Pid, cgmap);
    if (error)
        return error;

    if (cgmap.find("freezer") == cgmap.end())
        return TError(EError::Unknown, "Can't determine freezer cgroup of client process");

    auto freezer = cgmap["freezer"];
    auto prefix = PORTO_ROOT_CONTAINER + "/";
    std::string name;

    if (freezer.length() > prefix.length() && freezer.substr(0, prefix.length()) == prefix)
        name = freezer.substr(prefix.length());
    else
        name = PORTO_ROOT_CONTAINER;

    std::shared_ptr<TContainer> container;
    error = holder.Get(name, container);
    if (error)
        return error;

    Container = container;
    return TError::Success();
}

std::string TClient::GetContainerName() const {
    auto c = Container.lock();
    PORTO_ASSERT(c);
    return c->GetName();
}

std::shared_ptr<TContainer> TClient::GetContainer() const {
    auto c = Container.lock();
    PORTO_ASSERT(c);
    return c;
}
