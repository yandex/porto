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

            TError err = LoadGroups();
            if (err) {
                L_WRN() << "Can't load supplementary group list" << cr.pid
                    << " : " << err << std::endl;
            }

            err = IdentifyContainer(holder);
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

TError TClient::LoadGroups() {
    TFile f("/proc/" + std::to_string(Pid) + "/status");

    std::vector<std::string> lines;
    TError error = f.AsLines(lines);
    if (error)
        return error;

    Cred.Groups.clear();
    for (auto &l : lines)
        if (l.compare(0, 8, "Groups:\t") == 0) {
            std::vector<std::string> groupsStr;

            error = SplitString(l.substr(8), ' ', groupsStr);
            if (error)
                return error;

            for (auto g : groupsStr) {
                int group;
                error = StringToInt(g, group);
                if (error)
                    return error;

                Cred.Groups.push_back(group);
            }

            break;
        }

    return TError::Success();
}

TError TClient::IdentifyContainer(TContainerHolder &holder) {
    std::shared_ptr<TContainer> c;
    TError err = holder.Get(Pid, c);
    if (err)
        return err;
    Container = c;
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

std::shared_ptr<TContainer> TClient::TryGetContainer() const {
    return Container.lock();
}

bool TClient::Readonly() {
    auto c = Container.lock();
    if (!c)
        return true;

    if (c->IsNamespaceIsolated())
        return false;

    return !Cred.IsPrivileged() && !Cred.MemberOf(CredConf.GetPortoGid());
}
