#include <algorithm>
#include <sstream>
#include <iomanip>

#include "rpc.hpp"
#include "client.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "config.hpp"
#include "protobuf.hpp"
#include "util/file.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
};

TClient::TClient(std::shared_ptr<TEpollLoop> loop, int fd) : TEpollSource(loop, fd) {
    SetState(EClientState::ReadingLength);
    if (config().log().verbose())
        L() << "Client connected " << Fd << std::endl;
}

TClient::~TClient() {
    if (config().log().verbose())
        L() << "Client disconnected " << Fd << std::endl;
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
    if (c)
        return c->GetName();
    else
        return "<deleted container>";
}

TError TClient::GetContainer(std::shared_ptr<TContainer> &container) const {
    container = Container.lock();
    if (!container)
        return TError(EError::ContainerDoesNotExist, "Can't find client container");
    return TError::Success();
}

bool TClient::Readonly() {
    auto c = Container.lock();
    if (!c)
        return true;

    if (c->IsNamespaceIsolated())
        return false;

    return !Cred.IsPrivileged() && !Cred.IsMemberOf(CredConf.GetPortoGid());
}

void TClient::SetState(EClientState state) {
    Pos = 0;
    Length = 0;

    State = state;
}

bool TClient::ReadRequest(rpc::TContainerRequest &req, bool &hangup) {
    if (config().daemon().blocking_read()) {
        InterruptibleInputStream InputStream(Fd);
        return ReadDelimitedFrom(&InputStream, &req);
    }

    while (State == EClientState::ReadingLength) {
        uint8_t byte;
        int ret = recv(Fd, &byte, sizeof(byte), MSG_DONTWAIT);
        if (ret <= 0)
            return false;

        Length |= (byte & 0x7f) << Pos;
        Pos += 7;

        if ((byte & 0x80) == 0) {
            if (Length > config().daemon().max_msg_len()) {
                L_WRN() << "Got oversized request " << Length << " from client " << Fd << std::endl;
                SetState(EClientState::ReadingLength);
                hangup = true;
                return false;
            }

            Request.Alloc(Length);
            SetState(EClientState::ReadingData);
        }
    }

    if (State == EClientState::ReadingData) {
        int ret = recv(Fd, (uint8_t *)Request.GetData() + Pos, Request.GetSize() - Pos, MSG_DONTWAIT);
        if (ret <= 0)
            return false;

        Pos += ret;

        if (Pos >= Request.GetSize()) {
            bool ret = req.ParseFromArray(Request.GetData(), Request.GetSize());
            if (!ret) {
                L_WRN() << "Couldn't parse request from client " << Fd << std::endl;
                hangup = true;
            }
            SetState(EClientState::ReadingLength);
            return ret;
        }
    }

    return false;
}

std::ostream& operator<<(std::ostream& stream, TClient& client) {
    if (client.FullLog) {
        client.FullLog = false;
        stream << client.Comm << "(" << client.Pid << ") "
            << client.Cred.UserAsString() << ":"
            << client.Cred.GroupAsString() << " "
            << client.GetContainerName();
    } else {
        stream << client.Comm << "(" << client.Pid << ")";
    }
    return stream;
}
