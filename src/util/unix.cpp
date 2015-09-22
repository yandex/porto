#include <string>
#include <algorithm>

#include "util/file.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/path.hpp"
#include "util/log.hpp"
#include "unix.hpp"

extern "C" {
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <libgen.h>
#include <sys/sysinfo.h>
#include <sys/prctl.h>
#include <poll.h>
#include <linux/capability.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
}

int RetryBusy(int times, int timeoMs, std::function<int()> handler) {
    int ret = 0;

    if (!times)
        times = 1;

    while (times-- > 0) {
        ret = handler();
        if (!ret || errno != EBUSY)
            return ret;
        if (usleep(timeoMs * 1000) < 0)
            return -1;
    }

    return ret;
}

int RetryFailed(int times, int timeoMs, std::function<int()> handler) {
    int ret = 0;

    if (!times)
        times = 1;

    while (times-- > 0) {
        ret = handler();

        if (ret == 0)
            return ret;
        if (usleep(timeoMs * 1000) < 0)
            return -1;
    }

    return ret;
}

int SleepWhile(int timeoMs, std::function<int()> handler) {
    const int resolution = 5;
    int times = timeoMs / resolution;

    if (!times)
        times = 0;

    return RetryFailed(times, resolution, handler);
}

pid_t GetPid() {
    return getpid();
}

pid_t GetPPid() {
    return getppid();
}

pid_t GetTid() {
    return syscall(SYS_gettid);
}

TError GetTaskParent(pid_t pid, pid_t &parent_pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    int res, ppid;
    FILE *file;

    file = fopen(path.c_str(), "r");
    if (!file)
        return TError(EError::Unknown, errno, "fopen(" + path + ")");
    res = fscanf(file, "%*d (%*[^)]) %*c %d", &ppid);
    fclose(file);
    if (res != 1)
        return TError(EError::Unknown, errno, "Cannot parse " + path);
    parent_pid = ppid;
    return TError::Success();
}

TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens) {
    struct dirent *de;
    FILE *file;
    DIR *dir;
    int child_pid, parent_pid;

    childrens.clear();

    dir = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (!dir)
        goto full_scan;

    while ((de = readdir(dir))) {
        file = fopen(("/proc/" + std::to_string(pid) + "/task/" +
                      std::string(de->d_name) + "/children").c_str(), "r");
        if (!file) {
            if (atoi(de->d_name) != pid)
                continue;
            closedir(dir);
            goto full_scan;
        }

        while (fscanf(file, "%d", &child_pid) == 1)
            childrens.push_back(child_pid);
        fclose(file);
    }
    closedir(dir);

    return TError::Success();

full_scan:
    dir = opendir("/proc");
    if (!dir)
        return TError(EError::Unknown, errno, "Cannot open /proc");

    while ((de = readdir(dir))) {
        file = fopen(("/proc/" + std::string(de->d_name) + "/stat").c_str(), "r");
        if (!file)
            continue;

        if (fscanf(file, "%d (%*[^)]) %*c %d", &child_pid, &parent_pid) == 2 &&
                parent_pid == pid)
            childrens.push_back(child_pid);
        fclose(file);
    }
    closedir(dir);
    return TError::Success();
}

size_t GetCurrentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

size_t GetTotalMemory() {
    struct sysinfo si;
    if (sysinfo(&si) < 0)
        return 0;

    return si.totalram;
}

int CreatePidFile(const std::string &path, const int mode) {
    TFile f(path, mode);

    return f.WriteStringNoAppend(std::to_string(getpid()));
}

void RemovePidFile(const std::string &path) {
    TFile f(path);
    if (f.Exists())
        (void)f.Remove();
}

static __thread std::string *processName;

void SetProcessName(const std::string &name) {
    delete processName;
    processName = nullptr;
    prctl(PR_SET_NAME, (void *)name.c_str());
}

void SetDieOnParentExit(int sig) {
    (void)prctl(PR_SET_PDEATHSIG, sig, 0, 0, 0);
}

std::string GetProcessName() {
    if (!processName) {
        char name[17];

        if (prctl(PR_GET_NAME, (void *)name) < 0)
            strncpy(name, program_invocation_short_name, sizeof(name));

        processName = new std::string(name);
    }

    return *processName;
}

TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap) {
    TFile f("/proc/" + std::to_string(pid) + "/cgroup");
    std::vector<std::string> lines;
    TError error = f.AsLines(lines);
    if (error)
        return error;

    std::vector<std::string> tokens;
    for (auto l : lines) {
        tokens.clear();
        error = SplitString(l, ':', tokens, 3);
        if (error)
            return error;
        cgmap[tokens[1]] = tokens[2];
    }

    return TError::Success();
}

std::string GetHostName() {
    char buf[256];
    int ret = gethostname(buf, sizeof(buf));
    if (ret < 0)
        return "";
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

TError SetHostName(const std::string &name) {
    int ret = sethostname(name.c_str(), name.length());
    if (ret < 0)
        return TError(EError::Unknown, errno, "sethostname(" + name + ")");

    return TError::Success();
}

bool FdHasEvent(int fd) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;

    (void)poll(&pfd, 1, 0);
    return pfd.revents != 0;
}

TError DropBoundedCap(int cap) {
    if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0)
        return TError(EError::Unknown, errno,
                      "prctl(PR_CAPBSET_DROP, " + std::to_string(cap) + ")");

    return TError::Success();
}

TError SetCap(uint64_t effective, uint64_t permitted, uint64_t inheritable) {
    cap_user_header_t hdrp = (cap_user_header_t)malloc(sizeof(*hdrp));
    cap_user_data_t datap = (cap_user_data_t)malloc(sizeof(*datap) * 2);
    if (!hdrp || !datap)
        throw std::bad_alloc();

    hdrp->version = _LINUX_CAPABILITY_VERSION_3;
    hdrp->pid = getpid();
    datap[0].effective = (uint32_t)effective;
    datap[1].effective = (uint32_t)(effective >> 32);
    datap[0].permitted = (uint32_t)permitted;
    datap[1].permitted = (uint32_t)(permitted >> 32);
    datap[0].inheritable = (uint32_t)inheritable;
    datap[1].inheritable = (uint32_t)(inheritable >> 32);

    if (syscall(SYS_capset, hdrp, datap) < 0) {
        int err = errno;
        free(hdrp);
        free(datap);
        return TError(EError::Unknown, err, "capset(" +
                      std::to_string(effective) + ", " +
                      std::to_string(permitted) + ", " +
                      std::to_string(inheritable) + ")");
    }

    free(hdrp);
    free(datap);

    return TError::Success();
}

TScopedFd::TScopedFd(int fd) : Fd(fd) {
}

TScopedFd::~TScopedFd() {
    if (Fd >= 0)
        close(Fd);
}

int TScopedFd::GetFd() const {
    return Fd;
}

TScopedFd &TScopedFd::operator=(int fd) {
    if (Fd >= 0)
        close(Fd);

    Fd = fd;

    return *this;
}

TScopedMem::TScopedMem() : Size(0) {
}

TScopedMem::TScopedMem(size_t size) : Size(size) {
    Alloc(size);
}

void TScopedMem::Alloc(size_t size) {
    Size = size;
    Free();
    if (size) {
        Data = malloc(size);
        PORTO_ASSERT(Data != nullptr);
    }
}

void TScopedMem::Free() {
    free(Data);
    Data = nullptr;
}

TScopedMem::~TScopedMem() {
    Free();
}

void *TScopedMem::GetData() {
    return Data;
}

size_t TScopedMem::GetSize() {
    return Size;
}

TError SetOomScoreAdj(int value) {
    TFile f("/proc/self/oom_score_adj");
    return f.WriteStringNoAppend(std::to_string(value));
}

int64_t GetBootTime() {
    std::vector<std::string> lines;
    TFile f("/proc/stat");
    if (f.AsLines(lines))
        return 0;

    for (auto &line : lines) {
        std::vector<std::string> cols;
        if (SplitString(line, ' ', cols))
            return 0;

        if (cols[0] == "btime") {
            int64_t val;
            if (StringToInt64(cols[1], val))
                return 0;
            return val;
        }
    }

    return 0;
}

void CloseFds(int max, const std::set<int> &except, bool openStd) {
    if (max < 0)
        max = getdtablesize();

    for (int i = 0; i < max; i++) {
        if (std::find(except.begin(), except.end(), i) != except.end())
            continue;

        close(i);

        if (i < 3 && openStd) {
            int fd = open("/dev/null", O_RDWR);
            if (fd != i)
                L_ERR() << "Got unexpected std fd " << fd << ", expected " << i << std::endl;
        }
    }
}

TError AllocLoop(const TPath &path, size_t size) {
    TError error;
    TScopedFd fd;
    int status;
    std::vector<std::string> lines;

    fd = open(path.ToString().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + path.ToString() + ")");

    int ret = ftruncate(fd.GetFd(), size);
    if (ret < 0) {
        error = TError(EError::Unknown, errno, "truncate(" + path.ToString() + ")");
        goto remove_file;
    }

    fd = -1;

    error = Run({ "mkfs.ext4", "-F", "-F", path.ToString()}, status);
    if (error)
        goto remove_file;

    if (status) {
        error = TError(EError::Unknown, error.GetErrno(), "mkfs returned " + std::to_string(status) + ": " + error.GetMsg());
        goto remove_file;
    }

    return TError::Success();

remove_file:
    TFile f(path);
    (void)f.Remove();

    return error;
}

TError Run(const std::vector<std::string> &command, int &status, bool stdio) {
    int pid = fork();
    if (pid < 0) {
        return TError(EError::Unknown, errno, "fork()");
    } else if (pid > 0) {
        int ret;
retry:
        ret = waitpid(pid, &status, 0);
        if (ret < 0) {
            if (errno == EINTR)
                goto retry;
            return TError(EError::Unknown, errno, "waitpid(" + std::to_string(pid) + ")");
        }
    } else {
        SetDieOnParentExit(SIGKILL);
        if (!stdio) {
            CloseFds(-1, {});
            open("/dev/null", O_RDONLY);
            open("/dev/null", O_WRONLY);
            open("/dev/null", O_WRONLY);
        }

        char **p = (char **)malloc(sizeof(*p) * (command.size() + 1));
        for (size_t i = 0; i < command.size(); i++)
            p[i] = strdup(command[i].c_str());
        p[command.size()] = nullptr;

        execvp(command[0].c_str(), p);
        _exit(EXIT_FAILURE);
    }

    return TError::Success();
}

TError Popen(const std::string &cmd, std::vector<std::string> &lines) {
    FILE *f = popen(cmd.c_str(), "r");
    if (f == nullptr)
        return TError(EError::Unknown, errno, "Can't execute " + cmd);

    char *line = nullptr;
    size_t n = 0;

    while (getline(&line, &n, f) >= 0)
        lines.push_back(line);

    fclose(f);
    free(line);

    return TError::Success();
}

// https://github.com/lxc/lxc/commit/2d489f9e87fa0cccd8a1762680a43eeff2fe1b6e
TError PivotRoot(const TPath &rootfs) {
    TScopedFd oldroot, newroot;

    oldroot = open("/", O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (oldroot.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(/)");

    newroot = open(rootfs.ToString().c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (newroot.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + rootfs.ToString() + ")");

    if (fchdir(newroot.GetFd()))
        return TError(EError::Unknown, errno, "fchdir(newroot)");

    if (syscall(__NR_pivot_root, ".", "."))
        return TError(EError::Unknown, errno, "pivot_root()");

    if (fchdir(oldroot.GetFd()) < 0)
        return TError(EError::Unknown, errno, "fchdir(oldroot)");

    if (umount2(".", MNT_DETACH) < 0)
        return TError(EError::Unknown, errno, "umount2(.)");

    if (fchdir(newroot.GetFd()) < 0)
        return TError(EError::Unknown, errno, "fchdir(newroot) reenter");

    return TError::Success();
}

size_t GetNumCores() {
    long ncores = sysconf(_SC_NPROCESSORS_CONF);
    if (ncores <= 0) {
        TError error(EError::Unknown, "Can't get number of CPU cores");
        L_ERR() << error << std::endl;
        return 1;
    }

    return (size_t)ncores;
}

TError PackTarball(const TPath &tar, const TPath &path) {
    int status;

    TError error = Run({ "tar", "--one-file-system", "--numeric-owner",
                         "--sparse",  "--transform", "s:^./::",
                         "-cpaf", tar.ToString(), "-C", path.ToString(), "." }, status);
    if (error)
        return error;

    if (status)
        return TError(EError::Unknown, "Can't create tar " + std::to_string(status));

    return TError::Success();
}

TError UnpackTarball(const TPath &tar, const TPath &path) {
    int status;

    TError error = Run({ "tar", "--numeric-owner", "-pxf", tar.ToString(), "-C", path.ToString() }, status);
    if (error)
        return error;

    if (status)
        return TError(EError::Unknown, "Can't execute tar " + std::to_string(status));

    return TError::Success();
}

TError CopyRecursive(const TPath &src, const TPath &dst) {
    int status;

    TError error = Run({ "cp", "--archive", "--force",
                               "--one-file-system", "--no-target-directory",
                               src.ToString(), dst.ToString() }, status);
    if (error)
        return error;

    if (status)
        return TError(EError::Unknown, "Can't execute cp " + std::to_string(status));

    return TError::Success();
}

void DumpMallocInfo() {
    struct mallinfo mi = mallinfo();
    L() << "Total non-mapped bytes (arena):\t" << mi.arena << std::endl;
    L() << "# of free chunks (ordblks):\t" << mi.ordblks << std::endl;
    L() << "# of free fastbin blocks (smblks):\t" << mi.smblks << std::endl;
    L() << "# of mapped regions (hblks):\t" << mi.hblks << std::endl;
    L() << "Bytes in mapped regions (hblkhd):\t" << mi.hblkhd << std::endl;
    L() << "Max. total allocated space (usmblks):\t" << mi.usmblks << std::endl;
    L() << "Free bytes held in fastbins (fsmblks):\t" << mi.fsmblks << std::endl;
    L() << "Total allocated space (uordblks):\t" << mi.uordblks << std::endl;
    L() << "Total free space (fordblks):\t" << mi.fordblks << std::endl;
    L() << "Topmost releasable block (keepcost):\t" << mi.keepcost << std::endl;
}

std::string GetCwd() {
    char *cwd = get_current_dir_name();
    std::string s(cwd);
    free(cwd);
    return s;
}

void TUnixSocket::Close() {
    if (SockFd >= 0)
        close(SockFd);
    SockFd = -1;
}

void TUnixSocket::operator=(int sock) {
    Close();
    SockFd = sock;
}

TUnixSocket& TUnixSocket::operator=(TUnixSocket&& Sock) {
    Close();
    SockFd = Sock.SockFd;
    Sock.SockFd = -1;
    return *this;
}

TError TUnixSocket::SocketPair(TUnixSocket &sock1, TUnixSocket &sock2) {
    int sockfds[2];
    int ret, one = 1;

    ret = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockfds);
    if (ret)
        return TError(EError::Unknown, errno, "socketpair(AF_UNIX)");

    if (setsockopt(sockfds[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(int)) < 0 ||
        setsockopt(sockfds[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(int)) < 0) {
        close(sockfds[0]);
        close(sockfds[1]);
        return TError(EError::Unknown, errno, "setsockopt(SO_PASSCRED)");
    }

    sock1 = sockfds[0];
    sock2 = sockfds[1];
    return TError::Success();
}

TError TUnixSocket::SendInt(int val) const {
    ssize_t ret;

    ret = write(SockFd, &val, sizeof(val));
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot send int");
    if (ret != sizeof(val))
        return TError(EError::Unknown, "partial read of int: " + std::to_string(ret));
    return TError::Success();
}

TError TUnixSocket::RecvInt(int &val) const {
    ssize_t ret;

    ret = read(SockFd, &val, sizeof(val));
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot receive int");
    if (ret != sizeof(val))
        return TError(EError::Unknown, "partial read of int: " + std::to_string(ret));
    return TError::Success();
}

TError TUnixSocket::SendPid(pid_t pid) const {
    struct iovec iovec = {
        .iov_base = &pid,
        .iov_len = sizeof(pid),
    };
    char buffer[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    struct ucred *ucred = (struct ucred *)CMSG_DATA(cmsg);
    ssize_t ret;

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    ucred->pid = pid;
    ucred->uid = getuid();
    ucred->gid = getgid();

    ret = sendmsg(SockFd, &msghdr, 0);
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot report real pid");
    if (ret != sizeof(pid))
        return TError(EError::Unknown, "partial sendmsg: " + std::to_string(ret));
    return TError::Success();
}

TError TUnixSocket::RecvPid(pid_t &pid, pid_t &vpid) const {
    struct iovec iovec = {
        .iov_base = &vpid,
        .iov_len = sizeof(vpid),
    };
    char buffer[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    struct ucred *ucred = (struct ucred *)CMSG_DATA(cmsg);
    ssize_t ret;

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));

    ret = recvmsg(SockFd, &msghdr, 0);
    if (ret < 0)
        return TError(EError::Unknown, errno, "cannot receive real pid");
    if (ret != sizeof(pid))
        return TError(EError::Unknown, "partial recvmsg: " + std::to_string(ret));
    cmsg = CMSG_FIRSTHDR(&msghdr);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_CREDENTIALS)
        return TError(EError::Unknown, "no credetntials after recvmsg");
    pid = ucred->pid;
    return TError::Success();
}

TError TUnixSocket::SendError(const TError &error) const {
    return error.Serialize(SockFd);
}

TError TUnixSocket::RecvError() const {
    TError error;

    TError::Deserialize(SockFd, error);
    return error;
}
