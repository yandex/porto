#include "client.hpp"

#include "util/file.hpp"

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

TError TClient::Identify(bool full) {
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
        }

        Cred.Uid = cr.uid;
        Cred.Gid = cr.gid;

        return TError::Success();
    } else {
        return TError(EError::Unknown, "Can't identify client");
    }
}
