#include <algorithm>
#include <sstream>
#include <iomanip>

#include "rpc.hpp"
#include "client.hpp"
#include "container.hpp"
#include "statistics.hpp"
#include "holder.hpp"
#include "config.hpp"
#include "protobuf.hpp"
#include "util/file.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

#include <google/protobuf/io/coded_stream.h>

extern "C" {
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
};

TClient::TClient(std::shared_ptr<TEpollLoop> loop, int fd) : TEpollSource(loop, fd) {
    ConnectionTime = GetCurrentTimeMs();
    Statistics->Clients++;
}

TClient::~TClient() {
    CloseConnection();
    Statistics->Clients--;
}

void TClient::CloseConnection() {
    if (Fd >= 0) {
        ConnectionTime = GetCurrentTimeMs() - ConnectionTime;
        if (config().log().verbose())
            L() << "Client " << Fd << " disconnected : " << *this
                << " : " << ConnectionTime << " ms" <<  std::endl;
        close(Fd);
        Fd = -1;
    }

    for (auto &weakCt: WeakContainers) {
        auto container = weakCt.lock();
        if (container)
            container->DestroyWeak();
    }
    WeakContainers.clear();
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

uint64_t TClient::GetRequestTimeMs() {
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
    TError err = holder.FindTaskContainer(Pid, c);
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

TError TClient::ReadRequest(rpc::TContainerRequest &request) {
    if (config().daemon().blocking_read()) {
        InterruptibleInputStream InputStream(Fd);
        ReadDelimitedFrom(&InputStream, &request);
        return TError::Success();
    }

    if (Offset >= Buffer.size())
        Buffer.resize(Offset + 4096);

    ssize_t len = recv(Fd, &Buffer[Offset], Buffer.size() - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        return TError(EError::Unknown, len ? errno : EIO, "recv request failed");

    if (Length && Offset < Length)
        return TError::Queued();

    google::protobuf::io::CodedInputStream input(&Buffer[0], Offset);

    uint32_t length;
    if (!input.ReadVarint32(&length))
        return TError::Queued();

    if (!Length) {
        if (length > config().daemon().max_msg_len())
            return TError(EError::Unknown, "oversized request: " + std::to_string(length));

        Length = length + google::protobuf::io::CodedOutputStream::VarintSize32(length);
        if (Buffer.size() < Length)
            Buffer.resize(Length);

        if (Offset < Length)
            return TError::Queued();
    }

    if (!request.ParseFromCodedStream(&input))
        return TError(EError::Unknown, "cannot parse request");

    if (Offset > Length)
        return TError(EError::Unknown, "garbage after request");

    return EpollLoop->StopInput(Fd);
}

TError TClient::SendResponse(bool first) {
    ssize_t len = send(Fd, &Buffer[Offset], Length - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        return TError(EError::Unknown, len ? errno : EIO, "send response failed");

    if (Offset >= Length) {
        Length = Offset = 0;
        return EpollLoop->StartInput(Fd);
    }

    if (first)
        return EpollLoop->StartOutput(Fd);

    return TError::Success();
}

TError TClient::QueueResponse(rpc::TContainerResponse &response) {
    uint32_t length = response.ByteSize();
    size_t lengthSize = google::protobuf::io::CodedOutputStream::VarintSize32(length);

    Offset = 0;
    Length = lengthSize + length;

    if (Buffer.size() < Length)
        Buffer.resize(Length);

    google::protobuf::io::CodedOutputStream::WriteVarint32ToArray(length, &Buffer[0]);
    if (!response.SerializeToArray(&Buffer[lengthSize], length))
        return TError(EError::Unknown, "cannot serialize response");

    return SendResponse(true);
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
