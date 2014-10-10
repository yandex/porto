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
static pair<string, int> nameToType[] = {
    //{ "ns/user", CLONE_NEWUSER },
    { "ns/ipc", CLONE_NEWIPC },
    { "ns/uts", CLONE_NEWUTS },
    { "ns/net", CLONE_NEWNET },
    { "ns/pid", CLONE_NEWPID },
    { "ns/mnt", CLONE_NEWNS },
};

TError TNamespaceSnapshot::Create(int pid) {
    Destroy();

    int nr = 0;
    for (auto &pair : nameToType) {
        std::string path = "/proc/" + std::to_string(pid) + "/" + pair.first;

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT)
            continue;

        if (fd < 0) {
            Destroy();
            return TError(EError::Unknown, errno, "Can't open namespace fd");
        }

        nsToFd[pair.second] = fd;
        nr++;
    }

    if (!nr) {
        Destroy();
        return TError(EError::Unknown, "Can't find any namespace");
    }

    return TError::Success();
}

void TNamespaceSnapshot::Destroy() {
    for (auto &pair : nsToFd)
        close(pair.second);
    nsToFd.clear();
}

TError TNamespaceSnapshot::Attach() {
    for (auto &pair : nsToFd) {
        if (setns(pair.second, pair.first))
            return TError(EError::Unknown, errno, "Can't set namespace");
    }

    return TError::Success();
}
