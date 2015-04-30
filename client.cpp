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

size_t TClient::GetRequestStartMs() const {
    return RequestStartMs;
}

void TClient::SetRequestStartMs(size_t start) {
    RequestStartMs = start;
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

            comm.erase(remove(comm.begin(), comm.end(), '\n'), comm.end());

            Pid = cr.pid;
            Comm = comm;

            IdentifyContainer(holder);
        }

        Cred.Uid = cr.uid;
        Cred.Gid = cr.gid;

        return TError::Success();
    } else {
        return TError(EError::Unknown, "Can't identify client");
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
    auto prefix = "/" + PORTO_ROOT_CGROUP + "/";
    std::string name = "/";

    if (freezer.length() > prefix.length() && freezer.substr(0, prefix.length()) == prefix)
        name = freezer.replace(0, prefix.length(), "");

    std::shared_ptr<TContainer> container;
    if (!holder.Get(name, container))
        Container = container;

    return TError::Success();
}

std::string TClient::GetContainerName() const {
    auto c = Container.lock();
    if (c)
        return c->GetName();
    return "/";
}

std::shared_ptr<TContainer> TClient::GetContainer() const {
    return Container.lock();
}
