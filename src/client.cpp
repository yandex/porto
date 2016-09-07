#include <algorithm>
#include <sstream>
#include <iomanip>

#include "rpc.hpp"
#include "client.hpp"
#include "container.hpp"
#include "property.hpp"
#include "statistics.hpp"
#include "holder.hpp"
#include "config.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

#include <google/protobuf/io/coded_stream.h>

extern "C" {
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
};

TClient SystemClient("<system>");
__thread TClient *CurrentClient = nullptr;

TClient::TClient(std::shared_ptr<TEpollLoop> loop) : TEpollSource(loop, -1) {
    ConnectionTime = GetCurrentTimeMs();
    Statistics->Clients++;
}

TClient::TClient(const std::string &special) {
    Cred = TCred(RootUser, RootGroup);
    Comm = special;
    AccessLevel = EAccessLevel::Internal;
}

TClient::~TClient() {
    CloseConnection();
    if (AccessLevel != EAccessLevel::Internal)
        Statistics->Clients--;
}

TError TClient::AcceptConnection(TContext &context, int listenFd) {
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;
    TError error;

    peer_addr_size = sizeof(struct sockaddr_un);
    Fd = accept4(listenFd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (Fd < 0) {
        error = TError(EError::Unknown, errno, "accept4()");
        if (error.GetErrno() != EAGAIN)
            L_WRN() << "Cannot accept client: " << error << std::endl;
        return error;
    }

    error = IdentifyClient(*context.Cholder, true);
    if (error) {
        close(Fd);
        Fd = -1;
        return error;
    }

    if (Verbose)
        L() << "Client connected: " << *this << std::endl;

    return TError::Success();
}

void TClient::CloseConnection() {
    TScopedLock lock(Mutex);

    if (Fd >= 0) {
        EpollLoop->RemoveSource(Fd);
        ConnectionTime = GetCurrentTimeMs() - ConnectionTime;
        if (Verbose)
            L() << "Client disconnected: " << *this
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

void TClient::StartRequest() {
    RequestStartMs = GetCurrentTimeMs();
    PORTO_ASSERT(CurrentClient == nullptr);
    CurrentClient = this;
}

void TClient::FinishRequest() {
    PORTO_ASSERT(CurrentClient == this);
    CurrentClient = nullptr;
}

uint64_t TClient::GetRequestTimeMs() {
    return GetCurrentTimeMs() - RequestStartMs;
}

TError TClient::IdentifyClient(TContainerHolder &holder, bool initial) {
    std::shared_ptr<TContainer> ct;
    struct ucred cr;
    socklen_t len = sizeof(cr);
    TError error;

    if (getsockopt(Fd, SOL_SOCKET, SO_PEERCRED, &cr, &len))
        return TError(EError::Unknown, errno, "Cannot identify client: getsockopt() failed");

    /* check that request from the same pid and container is still here */
    if (!initial && Pid == cr.pid && TaskCred.Uid == cr.uid &&
            TaskCred.Gid == cr.gid && !ClientContainer.expired())
        return TError::Success();

    TaskCred.Uid = cr.uid;
    TaskCred.Gid = cr.gid;
    Pid = cr.pid;

    error = holder.FindTaskContainer(Pid, ct);
    if (error && error.GetErrno() != ENOENT)
        L_WRN() << "Cannot identify container of pid " << Pid
                << " : " << error << std::endl;
    if (error)
        return error;

    AccessLevel = ct->AccessLevel;
    for (auto p = ct->Parent; p; p = p->Parent)
        AccessLevel = std::min(AccessLevel, p->AccessLevel);

    if (AccessLevel == EAccessLevel::None)
        return TError(EError::Permission,
                      "Porto disabled in container " + ct->GetName());

    ClientContainer = ct;

    error = TPath("/proc/" + std::to_string(Pid) + "/comm").ReadAll(Comm, 64);
    if (error)
        Comm = "<unknown process>";
    else
        Comm.resize(Comm.length() - 1); /* cut \n at the end */

    if (ct->IsRoot()) {
        Cred.Uid = cr.uid;
        Cred.Gid = cr.gid;
        error = LoadGroups();
        if (error && error.GetErrno() != ENOENT)
            L_WRN() << "Cannot load supplementary group list" << Pid
                    << " : " << error << std::endl;
    } else {
        /* requests from containers are executed in behalf of their owners */
        Cred = ct->OwnerCred;
    }

    if (Cred.IsRootUser()) {
        if (AccessLevel == EAccessLevel::Normal)
            AccessLevel = EAccessLevel::SuperUser;
    } else if (!Cred.IsMemberOf(PortoGroup)) {
        if (AccessLevel >= EAccessLevel::ReadOnly)
            AccessLevel = EAccessLevel::ReadOnly;
    }

    return TError::Success();
}

TError TClient::LoadGroups() {
    std::vector<std::string> lines;
    TError error = TPath("/proc/" + std::to_string(Pid) + "/status").ReadLines(lines);
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

std::string TClient::GetContainerName() const {
    auto c = ClientContainer.lock();
    if (c)
        return c->GetName();
    else
        return "<deleted container>";
}

TError TClient::ComposeRelativeName(const TContainer &target,
                                    std::string &relative_name) const {
    std::string name = target.GetName();
    auto base = ClientContainer.lock();
    if (!base)
        return TError(EError::ContainerDoesNotExist, "Cannot find client container");
    std::string ns = base->GetPortoNamespace();

    if (target.IsRoot()) {
        relative_name = ROOT_CONTAINER;
        return TError::Success();
    }

    if (ns == "") {
        relative_name = name;
        return TError::Success();
    }

    if (name.length() <= ns.length() || name.compare(0, ns.length(), ns) != 0)
        return TError(EError::ContainerDoesNotExist,
                "Cannot access container " + name + " from namespace " + ns);

    relative_name = name.substr(ns.length());
    return TError::Success();
}

TError TClient::ResolveRelativeName(const std::string &relative_name,
                                    std::string &absolute_name,
                                    bool resolve_meta) const {
    auto base = ClientContainer.lock();
    if (!base)
        return TError(EError::ContainerDoesNotExist, "Cannot find client container");
    std::string ns = base->GetPortoNamespace();

    /* FIXME get rid of this crap */
    if (!resolve_meta && (relative_name == DOT_CONTAINER ||
                          relative_name == PORTO_ROOT_CONTAINER ||
                          relative_name == ROOT_CONTAINER))
        return TError(EError::Permission, "System containers are read only");

    if (relative_name == ROOT_CONTAINER ||
            relative_name == PORTO_ROOT_CONTAINER)
        absolute_name = relative_name;
    else if (relative_name == SELF_CONTAINER)
        absolute_name = base->GetName();
    else if (StringStartsWith(relative_name,
                std::string(SELF_CONTAINER) + "/")) {
        absolute_name = (base->IsRoot() ? "" : base->GetName() + "/") +
            relative_name.substr(std::string(SELF_CONTAINER).length() + 1);
    } else if (StringStartsWith(relative_name,
                std::string(PORTO_ROOT_CONTAINER) + "/")) {
        absolute_name = relative_name.substr(
                std::string(PORTO_ROOT_CONTAINER).length() + 1);
        if (!StringStartsWith(absolute_name, ns))
            return TError(EError::Permission,
                    "Absolute container name out of current namespace");
    } else if (relative_name == DOT_CONTAINER) {
        size_t off = ns.rfind('/');
        if (off != std::string::npos)
            absolute_name = ns.substr(0, off);
        else
            absolute_name = PORTO_ROOT_CONTAINER;
    } else
        absolute_name = ns + relative_name;

    return TError::Success();
}

TPath TClient::ComposePath(const TPath &path) {
    auto base = ClientContainer.lock();
    if (!base)
        return TPath();
    return base->RootPath.InnerPath(path);
}

TPath TClient::ResolvePath(const TPath &path) {
    auto base = ClientContainer.lock();
    if (!base)
        return TPath();
    return base->RootPath / path;
}

bool TClient::IsSuperUser(void) const {
    return AccessLevel >= EAccessLevel::SuperUser;
}

TError TClient::GetClientContainer(std::shared_ptr<TContainer> &container) const {
    container = ClientContainer.lock();
    if (!container)
        return TError(EError::ContainerDoesNotExist, "Cannot find client container");
    return TError::Success();
}

TError TClient::CanControl(const TCred &other) {

    if (AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "No write access at all");

    if (IsSuperUser() || Cred.Uid == other.Uid)
        return TError::Success();

    /* Everybody can control users from group porto-containers */
    if (other.IsMemberOf(PortoCtGroup))
        return TError::Success();

    /* Load group $USER-containers lazily */
    if (!UserCtGroup && GroupId(Cred.User() + USER_CT_SUFFIX, UserCtGroup))
        UserCtGroup = NoGroup;

    if (other.IsMemberOf(UserCtGroup))
        return TError::Success();

    return TError(EError::Permission, "User " + Cred.ToString() +
                                      " cannot control " + other.ToString());
}

TError TClient::CanControl(const TContainer &ct, bool createChild) {

    if (AccessLevel < EAccessLevel::ChildOnly)
        return TError(EError::Permission, "No write access at all");

    if (!createChild || !ct.IsPortoRoot()) {
        TError error = CanControl(ct.OwnerCred);
        if (error)
            return error;
    }

    if (AccessLevel > EAccessLevel::ChildOnly)
        return TError::Success();

    auto base = ClientContainer.lock();
    while (base && base->AccessLevel != EAccessLevel::ChildOnly)
        base = base->Parent;

    if (!base)
        return TError(EError::Permission, "Base for child-only not found");

    if ((createChild && base.get() == &ct) || ct.IsChildOf(*base))
        return TError::Success();

    return TError(EError::Permission, "Not a child container: " + ct.GetName());
}

TError TClient::ReadRequest(rpc::TContainerRequest &request) {
    TScopedLock lock(Mutex);

    if (Processing) {
        L_WRN() << "Client request before response: " << *this << std::endl;
        return TError::Success();
    }

    if (Fd < 0)
        return TError(EError::Unknown, "Connection closed");

    if (Offset >= Buffer.size())
        Buffer.resize(Offset + 4096);

    ssize_t len = recv(Fd, &Buffer[Offset], Buffer.size() - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0)
        return TError(EError::Unknown, "recv return zero");
    else if (errno != EAGAIN && errno != EWOULDBLOCK)
        return TError(EError::Unknown, errno, "recv request failed");

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
            Buffer.resize(Length + 4096);

        if (Offset < Length)
            return TError::Queued();
    }

    if (!request.ParseFromCodedStream(&input))
        return TError(EError::Unknown, "cannot parse request");

    if (Offset > Length)
        return TError(EError::Unknown, "garbage after request");

    Processing = true;
    return EpollLoop->StopInput(Fd);
}

TError TClient::SendResponse(bool first) {
    TScopedLock lock(Mutex);

    if (Fd < 0)
        return TError::Success(); /* Connection closed */

    ssize_t len = send(Fd, &Buffer[Offset], Length - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0) {
        if (!first)
            return TError(EError::Unknown, "send return zero");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK)
        return TError(EError::Unknown, errno, "send response failed");

    if (Offset >= Length) {
        Length = Offset = 0;
        Processing = false;
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
        stream << client.Fd << ":" <<  client.Comm << "(" << client.Pid << ") "
               << client.Cred << " " << client.GetContainerName();
    } else {
        stream << client.Fd << ":" << client.Comm << "(" << client.Pid << ")";
    }
    return stream;
}
