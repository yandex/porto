#include "namespace.hpp"
#include "util/file.hpp"

using std::pair;
using std::string;

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
}

// order is important
static pair<string, int> nameToType[TNamespaceSnapshot::nrNs] = {
    //{ "ns/user", CLONE_NEWUSER },
    { "ns/ipc", CLONE_NEWIPC },
    { "ns/uts", CLONE_NEWUTS },
    { "ns/net", CLONE_NEWNET },
    { "ns/pid", CLONE_NEWPID },
    { "ns/mnt", CLONE_NEWNS },
};

TError TNamespaceSnapshot::OpenProcPidFd(int pid, string name, int &fd) {
    std::string path = "/proc/" + std::to_string(pid) + "/" + name;
    fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return TError(EError::Unknown, errno, "Can't open " + path);

    return TError::Success();
}

TError TNamespaceSnapshot::Open(int pid, bool only_mnt) {
    int nr = 0;
    TError error;

    Close();

    for (int i = 0; i < nrNs; i++) {
        auto pair = nameToType[i];

        if (only_mnt && pair.second != CLONE_NEWNS)
            continue;

        int fd;
        error = OpenProcPidFd(pid, pair.first, fd);
        if (error) {
            if (error.GetErrno() == ENOENT)
                continue;
            Close();
            return error;
        }

        nsFd[i] = fd;
        nr++;
    }

    if (!nr) {
        Close();
        return TError(EError::Unknown, "Can't find any namespace");
    }

    error = OpenProcPidFd(pid, "root", RootFd);
    if (error) {
        Close();
        return error;
    }

    error = OpenProcPidFd(pid, "cwd", CwdFd);
    if (error) {
        Close();
        return error;
    }

    return TError::Success();
}

TError TNamespaceSnapshot::Chroot() const {
    if (fchdir(RootFd) < 0)
        return TError(EError::Unknown, errno, "Can't change root directory: fchdir(" + std::to_string(RootFd) + ")");

    if (chroot(".") < 0)
        return TError(EError::Unknown, errno, "Can't change root directory chroot(" + std::to_string(RootFd) + ")");

    if (fchdir(CwdFd) < 0)
        return TError(EError::Unknown, errno, "Can't change working directory fchdir(" + std::to_string(CwdFd) + ")");

    return TError::Success();
}

void TNamespaceSnapshot::Close() {
    for (int i = 0; i < nrNs; i++)
        if (nsFd[i] >= 0) {
            close(nsFd[i]);
            nsFd[i] = -1;
        }
    if (RootFd >= 0) {
        close(RootFd);
        RootFd = -1;
    }
    if (CwdFd >= 0) {
        close(CwdFd);
        CwdFd = -1;
    }
}

TError TNamespaceSnapshot::Attach() const {
    for (int i = 0; i < nrNs; i++)
        if (nsFd[i] >= 0 && setns(nsFd[i], nameToType[i].second))
            return TError(EError::Unknown, errno, "Can't set namespace");

    return TError::Success();
}

bool TNamespaceSnapshot::Valid() const {
    return RootFd >= 0 && CwdFd >= 0;
}
