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

TError TNamespaceFd::Open(TPath path) {
    Close();
    Fd = open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (Fd < 0)
        return TError(EError::Unknown, errno, "Cannot open " + path.ToString());
    return TError::Success();
}

TError TNamespaceFd::Open(pid_t pid, std::string type) {
    return Open("/proc/" + std::to_string(pid) + "/" + type);
}

void TNamespaceFd::Close() {
    if (Fd >= 0) {
        close(Fd);
        Fd = -1;
    }
}

TError TNamespaceFd::SetNs(int type) const {
    if (Fd >= 0 && setns(Fd, type))
        return TError(EError::Unknown, errno, "Cannot set namespace");
    return TError::Success();
}

TError TNamespaceFd::Chdir() const {
    if (Fd >= 0 && fchdir(Fd))
        return TError(EError::Unknown, errno, "Cannot change cwd");
    return TError::Success();
}

TError TNamespaceFd::Chroot() const {
    if (Fd >= 0 && (fchdir(Fd) || chroot(".")))
        return TError(EError::Unknown, errno, "Cannot change root");
    return TError::Success();
}

bool TNamespaceFd::operator==(const TNamespaceFd &other) const {
    struct stat a, b;

    if (Fd < 0 || other.Fd < 0 || fstat(Fd, &a) || fstat(other.Fd, &b))
        return false;

    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

bool TNamespaceFd::operator!=(const TNamespaceFd &other) const {
    return !(*this == other);
}

// order is important
static pair<string, int> nameToType[TNamespaceSnapshot::nrNs] = {
    //{ "user", CLONE_NEWUSER },
    { "ipc", CLONE_NEWIPC },
    { "uts", CLONE_NEWUTS },
    { "net", CLONE_NEWNET },
    { "pid", CLONE_NEWPID },
    { "mnt", CLONE_NEWNS },
};

TError TNamespaceSnapshot::OpenProcPidFd(int pid, string name, int &fd) {
    std::string path = "/proc/" + std::to_string(pid) + "/" + name;
    fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return TError(EError::Unknown, errno, "Can't open " + path);

    return TError::Success();
}

TError TNamespaceSnapshot::Open(int pid, std::set<std::string> ns) {
    int nr = 0;
    TError error;

    Close();

    for (int i = 0; i < nrNs; i++) {
        auto name = nameToType[i].first;

        if (ns.find(name) == ns.end())
            continue;

        int fd;
        error = OpenProcPidFd(pid, "ns/" + name, fd);
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

bool TNamespaceSnapshot::HasNs(const std::string &ns) const {
    for (int i = 0; i < nrNs; i++) {
        auto name = nameToType[i].first;
        if (name == ns)
            return nsFd[i] >= 0;
    }

    return false;
}
