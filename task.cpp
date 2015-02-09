#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "task.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "subsystem.hpp"
#include "util/log.hpp"
#include "util/mount.hpp"
#include "util/folder.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/netlink.hpp"
#include "util/crc32.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <wordexp.h>
#include <grp.h>
#include <linux/kdev_t.h>
#include <net/if.h>
#include <linux/capability.h>
}

using std::stringstream;
using std::string;
using std::vector;
using std::map;

static int lastCap;

// TTaskEnv

TError TTaskEnv::Prepare() {
    if (Command.empty())
        return TError::Success();

    TError error = Cred.Parse(User, Group);
    if (error)
        return error;

    return TError::Success();
}

const char** TTaskEnv::GetEnvp() const {
    auto envp = new const char* [Environ.size() + 1];
    for (size_t i = 0; i < Environ.size(); i++)
        envp[i] = strdup(Environ[i].c_str());
    envp[Environ.size()] = NULL;

    return envp;
}

// TTask
void TTask::Syslog(const string &s) const {
    openlog("portod", LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_ERR, "%s", s.c_str());
    closelog();
}

TTask::~TTask() {
    if (!Env)
        return;

    if (Env->RemoveStdout &&
        Env->StdoutPath.GetType() != EFileType::Character &&
        Env->StdoutPath.GetType() != EFileType::Block) {
        TFile out(Env->StdoutPath);
        TError error = out.Remove();
        if (error)
            L_ERR() << "Can't remove task stdout " << Env->StdoutPath << ": " << error << std::endl;
    }

    if (Env->RemoveStderr &&
        Env->StderrPath.GetType() != EFileType::Character &&
        Env->StderrPath.GetType() != EFileType::Block) {
        TFile err(Env->StderrPath);
        TError error = err.Remove();
        if (error)
            L_ERR() << "Can't remove task stderr " << Env->StderrPath << ": " << error << std::endl;
    }
}

void TTask::ReportPid(int pid) const {
    if (write(Wfd, &pid, sizeof(pid)) != sizeof(pid)) {
        Syslog("partial write of pid: " + std::to_string(pid));
    }
}

void TTask::Abort(const TError &error) const {
    TError ret = error.Serialize(Wfd);
    if (ret)
        Syslog(ret.GetMsg());
    exit(EXIT_FAILURE);
}

static int ChildFn(void *arg) {
    SetProcessName("portod-spawn-c");

    TLogger::DisableLog();
    TTask *task = static_cast<TTask*>(arg);
    TError error = task->ChildCallback();
    task->Abort(error);
    return EXIT_FAILURE;
}

TError TTask::ChildOpenStdFile(const TPath &path, int expected) {
    int ret = open(path.ToString().c_str(), O_CREAT | O_WRONLY | O_APPEND, 0700);
    if (ret < 0)
        return TError(EError::Unknown, errno,
                      "open(" + path.ToString() + ") -> " +
                      std::to_string(expected));

    if (ret != expected)
        return TError(EError::Unknown, EINVAL,
                      "open(" + path.ToString() + ") -> " +
                      std::to_string(expected) + ": unexpected fd");

    ret = fchown(ret, Env->Cred.Uid, Env->Cred.Gid);
    if (ret < 0)
        return TError(EError::Unknown, errno,
                      "fchown(" + path.ToString() + ") -> " +
                      std::to_string(expected));

    return TError::Success();
}

TError TTask::ChildReopenStdio() {
    Wfd = CloseAllFds(Wfd);
    if (Wfd < 0) {
        Syslog(string("close fds: ") + strerror(errno));
        /* there is no way of telling parent that we failed (because we
         * screwed up fds), so exit with some eye catching error code */
        exit(0xAA);
    }

    int ret = open(Env->StdinPath.ToString().c_str(), O_CREAT | O_RDONLY, 0700);
    if (ret < 0)
        return TError(EError::Unknown, errno, "open(0)");

    if (ret != 0)
        return TError(EError::Unknown, EINVAL, "open(0): unexpected fd");

    TError error = ChildOpenStdFile(Env->StdoutPath, 1);
    if (error)
        return error;
    error = ChildOpenStdFile(Env->StderrPath, 2);
    if (error)
        return error;

    return TError::Success();
}

TError TTask::ChildApplyCapabilities() {
    uint64_t effective, permitted, inheritable;

    if (!Env->Cred.IsRoot())
        return TError::Success();

    PORTO_ASSERT(lastCap != 0);

    effective = permitted = -1;
    inheritable = Env->Caps;

    TError error = SetCap(effective, permitted, inheritable);
    if (error)
        return error;

    for (int i = 0; i <= lastCap; i++) {
        if (!(Env->Caps & (1ULL << i)) && i != CAP_SETPCAP) {
            TError error = DropBoundedCap(i);
            if (error)
                return error;

        }
    }

    if (!(Env->Caps & (1ULL << CAP_SETPCAP))) {
        TError error = DropBoundedCap(CAP_SETPCAP);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildDropPriveleges() {
    if (setgid(Env->Cred.Gid) < 0)
        return TError(EError::Unknown, errno, "setgid()");

    if (initgroups(Env->User.c_str(), Env->Cred.Gid) < 0)
        return TError(EError::Unknown, errno, "initgroups()");

    if (setuid(Env->Cred.Uid) < 0)
        return TError(EError::Unknown, errno, "setuid()");

    return TError::Success();
}

TError TTask::ChildExec() {
    clearenv();

    for (auto &s : Env->Environ) {
        char *d = strdup(s.c_str());
        putenv(d);
    }

	wordexp_t result;

	int ret = wordexp(Env->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
    switch (ret) {
    case WRDE_BADCHAR:
        return TError(EError::Unknown, EINVAL, "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }");
    case WRDE_BADVAL:
        return TError(EError::Unknown, EINVAL, "wordexp(): undefined shell variable was referenced");
    case WRDE_CMDSUB:
        return TError(EError::Unknown, EINVAL, "wordexp(): command substitution is not supported");
    case WRDE_SYNTAX:
        return TError(EError::Unknown, EINVAL, "wordexp(): syntax error");
    default:
    case WRDE_NOSPACE:
        return TError(EError::Unknown, EINVAL, "wordexp(): error " + std::to_string(ret));
    case 0:
        break;
    }

    auto envp = Env->GetEnvp();
    if (config().log().verbose()) {
        Syslog("command=" + Env->Command);
        for (unsigned i = 0; i < result.we_wordc; i++)
            Syslog("argv[" + std::to_string(i) + "]=" + result.we_wordv[i]);
        for (unsigned i = 0; envp[i]; i++)
            Syslog("environ[" + std::to_string(i) + "]=" + envp[i]);
    }
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, (char *const *)envp);

    return TError(EError::Unknown, errno, string("execvpe(") + result.we_wordv[0] + ", " + std::to_string(result.we_wordc) + ", " + std::to_string(Env->Environ.size()) + ")");
}

TError TTask::ChildBindDns() {
    vector<string> files = { "/etc/hosts", "/etc/resolv.conf" };

    for (auto &file : files) {
        TMount mnt(file, Env->Root + file, "none", {});
        TError error = mnt.BindFile(true);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildBindDirectores() {
    for (TBindMap &bindMap : Env->BindMap) {
        TPath dest = Env->Root + bindMap.Dest;
        if (Env->Root == "/")
            dest = Env->Cwd + bindMap.Dest;

        TMount mnt(bindMap.Source, dest, "none", {});

        TError error;
        if (bindMap.Source.GetType() == EFileType::Directory)
            error = mnt.BindDir(bindMap.Rdonly);
        else
            error = mnt.BindFile(bindMap.Rdonly);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::CreateNode(const TPath &path, unsigned int mode, unsigned int dev) {
    if (mknod(path.ToString().c_str(), mode, dev) < 0)
        return TError(EError::Unknown, errno, "mknod(" + path.ToString() + ")");

    return TError::Success();
}

TError TTask::RestrictProc(bool restrictProcSys) {
    vector<string> dirs = { "/proc/sysrq-trigger", "/proc/irq", "/proc/bus" };

    if (restrictProcSys)
        dirs.push_back("/proc/sys");

    for (auto &path : dirs) {
        TMount mnt(Env->Root + path, Env->Root + path, "none", {});
        TError error = mnt.BindFile(true);
        if (error)
            return error;
    }

    TMount mnt("/dev/null", Env->Root + "/proc/kcore", "", {});
    TError error = mnt.Bind(false);
    if (error)
        return error;

    return TError::Success();
}

TError TTask::ChildMountRun() {
    TPath run = Env->Root + "/run";
    std::vector<std::string> subdirs;
    TFolder dir(run);
    if (!dir.Exists()) {
        TError error = dir.Create();
        if (error)
            return error;
    } else {
        TError error = dir.Items(EFileType::Directory, subdirs);
        if (error)
            return error;
    }

    TMount dev("tmpfs", run, "tmpfs", { "mode=755", "size=32m" });
    TError error = dev.MountDir(MS_NOSUID | MS_STRICTATIME);
    if (error)
        return error;

    for (auto name : subdirs) {
        TFolder d(run + "/" + name);
        TError error = d.Create();
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildMountDev() {
    struct {
        const std::string path;
        unsigned int mode;
        unsigned int dev;
    } node[] = {
        { "/dev/null",    0666 | S_IFCHR, MKDEV(1, 3) },
        { "/dev/zero",    0666 | S_IFCHR, MKDEV(1, 5) },
        { "/dev/full",    0666 | S_IFCHR, MKDEV(1, 7) },
        { "/dev/random",  0666 | S_IFCHR, MKDEV(1, 8) },
        { "/dev/urandom", 0666 | S_IFCHR, MKDEV(1, 9) },
    };

    TMount dev("tmpfs", Env->Root + "/dev", "tmpfs", { "mode=755", "size=32m" });
    TError error = dev.MountDir(MS_NOSUID | MS_STRICTATIME);
    if (error)
        return error;

    TMount devpts("devpts", Env->Root + "/dev/pts", "devpts",
                  { "newinstance", "ptmxmode=0666", "mode=620" ,"gid=5" });
    error = devpts.MountDir(MS_NOSUID | MS_NOEXEC);
    if (error)
        return error;

    for (size_t i = 0; i < sizeof(node) / sizeof(node[0]); i++) {
        error = CreateNode(Env->Root + node[i].path,
                           node[i].mode, node[i].dev);
        if (error)
            return error;
    }

    TPath ptmx = Env->Root + "/dev/ptmx";
    if (symlink("pts/ptmx", ptmx.ToString().c_str()) < 0)
        return TError(EError::Unknown, errno, "symlink(/dev/pts/ptmx)");

    TPath fd = Env->Root + "/dev/fd";
    if (symlink("/proc/self/fd", fd.ToString().c_str()) < 0)
        return TError(EError::Unknown, errno, "symlink(/dev/fd)");

    TFile f(Env->Root + "/dev/console", 0755);
    (void)f.Touch();

    return TError::Success();
}

TError TTask::ChildIsolateFs() {
    if (Env->Loop.Exists()) {
        TLoopMount m(Env->Loop, Env->Root, "ext4", Env->LoopDev);
        TError error = m.Mount();
        if (error)
            return error;
    }

    if (Env->Root.ToString() != "/") {
        TMount root(Env->Root, Env->Root, "none", {});
        TError error = root.BindDir(false, MS_SHARED);
        if (error)
            return error;
    }

    if (Env->Root.ToString() == "/") {
        TError error = ChildBindDirectores();
        if (error)
            return error;

        if (Env->RootRdOnly) {
            TMount root("/", "/", "none", {});
            TError error = root.BindDir(true);
            if (error)
                return error;
        }

        return TError::Success();
    }

    unsigned long defaultFlags = MS_NOEXEC | MS_NOSUID | MS_NODEV;
    unsigned long sysfsFlags = defaultFlags | MS_RDONLY;

    TMount sysfs("sysfs", Env->Root + "/sys", "sysfs", {});
    TError error = sysfs.MountDir(sysfsFlags);
    if (error)
        return error;

    TMount proc("proc", Env->Root + "/proc", "proc", {});
    error = proc.MountDir(defaultFlags);
    if (error)
        return error;

    bool privileged = Env->Cred.IsRoot();
    error = RestrictProc(!privileged);
    if (error)
        return error;

    error = ChildMountDev();
    if (error)
        return error;

    if (Env->Loop.Exists()) {
        error = ChildMountRun();
        if (error)
            return error;
    }

    TMount shm("shm", Env->Root + "/dev/shm", "tmpfs",
               { "mode=1777", "size=65536k" });
    error = shm.MountDir(defaultFlags);
    if (error)
        return error;

    if (Env->BindDns) {
        error = ChildBindDns();
        if (error)
            return error;
    }

    error = ChildBindDirectores();
    if (error)
        return error;

    if (Env->RootRdOnly == true) {
        TMount root(Env->Root, Env->Root, "none", {});
        TError error = root.Mount(MS_BIND | MS_REMOUNT | MS_RDONLY);
        if (error)
            return error;
    }

    error = Env->Root.Chdir();
    if (error)
        return error;

    error = Env->Root.Chroot();
    if (error)
        return error;

    TPath newRoot("/");
    return newRoot.Chdir();
}

TError TTask::EnableNet() {
    auto nl = std::make_shared<TNl>();
    if (!nl)
        throw std::bad_alloc();

    TError error = nl->Connect();
    if (error)
        return error;

    std::vector<std::string> devices = nl->FindLink(0);
    std::shared_ptr<TNlLink> gw = nullptr;
    for (auto &dev : devices) {
        auto link = std::make_shared<TNlLink>(nl, dev);

        TError error = link->Load();
        if (error)
            return error;

        error = link->Up();
        if (error)
            return error;

        if (Env->IpMap.find(dev) != Env->IpMap.end()) {
            auto ip = Env->IpMap[dev];

            if (!ip.Addr.IsEmpty()) {
                TError error = link->SetIpAddr(ip.Addr, ip.Prefix);
                if (error)
                    return error;
            }
        }

        if (!gw && !link->IsLoopback())
            gw = link;
    }

    if (!Env->DefaultGw.IsEmpty() && gw) {
        error = gw->SetDefaultGw(Env->DefaultGw);
        if (error)
            return error;
    }

    return TError::Success();
}

static std::string GenerateHw(const std::string &host, const std::string &name) {
    uint32_t n = Crc32(name);
    uint32_t h = Crc32(host);

    char buf[32];

    sprintf(buf, "02:%02x:%02x:%02x:%02x:%02x",
            (n & 0x000000FF) >> 0,
            (h & 0xFF000000) >> 24,
            (h & 0x00FF0000) >> 16,
            (h & 0x0000FF00) >> 8,
            (h & 0x000000FF) >> 0);

    return std::string(buf);
}

TError TTask::IsolateNet(int childPid) {
    auto nl = std::make_shared<TNl>();
    if (!nl)
        throw std::bad_alloc();

    TError error = nl->Connect();
    if (error)
        return error;


    for (auto &host : Env->NetCfg.Host) {
        auto link = std::make_shared<TNlLink>(nl, host.Dev);
        TError error = link->ChangeNs(host.Dev, childPid);
        if (error)
            return error;
    }

    std::string hostname = GetHostName();

    for (auto &mvlan : Env->NetCfg.MacVlan) {
        auto link = std::make_shared<TNlLink>(nl, "portomv0");

        (void)link->Remove();

        string hw = mvlan.Hw;
        if (hw == "" && Env->Hostname != "")
            hw = GenerateHw(Env->Hostname, mvlan.Master + mvlan.Name);

        if (config().network().debug())
            L() << "Using " << hw << " for " << mvlan.Master << " -> " << mvlan.Name << std::endl;

        TError error = link->AddMacVlan(mvlan.Master, mvlan.Type, hw, mvlan.Mtu);
        if (error)
            return error;

        error = link->ChangeNs(mvlan.Name, childPid);
        if (error)
            return error;
    }

    for (auto &veth : Env->NetCfg.Veth) {
        auto bridge = std::make_shared<TNlLink>(nl, veth.Bridge);
        TError error = bridge->Load();
        if (error)
            return error;

        string hw = veth.Hw;
        if (hw == "" && Env->Hostname != "")
            hw = GenerateHw(Env->Hostname, veth.Name + veth.Peer);

        if (config().network().debug())
            L() << "Using " << hw << " for " << veth.Name << " -> " << veth.Peer << std::endl;

        error = bridge->AddVeth(veth.Name, veth.Peer, hw, veth.Mtu, childPid);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildApplyLimits() {
    for (auto pair : Env->Rlimit) {
        int ret = setrlimit(pair.first, &pair.second);
        if (ret < 0)
            return TError(EError::Unknown, errno,
                          "setrlimit(" + std::to_string(pair.first) +
                          ", " + std::to_string(pair.second.rlim_cur) +
                          ":" + std::to_string(pair.second.rlim_max) + ")");
    }

    return TError::Success();
}

TError TTask::ChildSetHostname() {
    if (Env->Hostname == "" || Env->Root.ToString() == "/")
        return TError::Success();

    TFile f("/etc/hostname");
    if (f.Exists()) {
        string host = Env->Hostname + "\n";
        TError error = f.WriteStringNoAppend(host);
        if (error)
            return TError(EError::Unknown, error, "write(/etc/hostname)");
    }

    if (sethostname(Env->Hostname.c_str(), Env->Hostname.length()) < 0)
        return TError(EError::Unknown, errno, "sethostname()");

    return TError::Success();
}

TError TTask::ChildPrepareLoop() {
    if (Env->Loop.Exists()) {
        TFolder f(Env->Root);
        if (!f.Exists()) {
            TError error = f.Create(0755, true);
            if (error)
                return error;
        }
    }

    return TError::Success();
}

TError TTask::ChildCallback() {
    int ret;
    if (read(WaitParentRfd, &ret, sizeof(ret)) != sizeof(ret))
        return TError(EError::Unknown, errno, "partial read from child sync pipe");

    close(Rfd);
    ResetAllSignalHandlers();
    TError error = ChildApplyLimits();
    if (error)
        return error;

    if (setsid() < 0)
        return TError(EError::Unknown, errno, "setsid()");

    umask(0);

    if (Env->Isolate) {
        // remount proc so PID namespace works
        TMount proc("proc", "/proc", "proc", {});
        if (proc.MountDir())
            return TError(EError::Unknown, errno, "remount procfs");
    }

    // move to target cgroups
    for (auto cg : LeafCgroups) {
        auto error = cg.second->Attach(getpid());
        if (error)
            return TError(EError::Unknown, error, "cgroup attach");
    }

    if (Env->Isolate) {
        error = ChildPrepareLoop();
        if (error)
            return error;
    }

    if (!Env->NetCfg.Share) {
        error = EnableNet();
        if (error)
            return error;
    }

    error = ChildReopenStdio();
    if (error)
        return error;

    if (Env->Ns.Valid()) {
        error = Env->Ns.Chroot();
        if (error)
            return error;
    } else {
        error = ChildIsolateFs();
        if (error)
            return error;

        error = Env->Cwd.Chdir();
        if (error)
            return error;

        error = ChildSetHostname();
        if (error)
            return error;
    }

    error = ChildApplyCapabilities();
    if (error)
        return error;

    error = ChildDropPriveleges();
    if (error)
        return error;

    return ChildExec();
}

TError TTask::CreateCwd() {
    bool cleanup = Env->Cwd.ToString().find(config().container().tmp_dir()) == 0;

    Cwd = std::make_shared<TFolder>(Env->Cwd, cleanup);
    if (!Cwd->Exists()) {
        TError error = Cwd->Create(0755, true);
        if (error)
            return error;
        error = Env->Cwd.Chown(Env->User, Env->Group);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::Start() {
    int ret;
    int pfd[2], syncfd[2];

    if (Env->CreateCwd) {
        TError error = CreateCwd();
        if (error) {
            L_ERR() << "Can't create temporary cwd: " << error << std::endl;
            return error;
        }
    }

    ExitStatus = 0;

    ret = pipe2(pfd, O_CLOEXEC);
    if (ret) {
        TError error(EError::Unknown, errno, "pipe2(pdf)");
        L_ERR() << "Can't create communication pipe for child: " << error << std::endl;
        return error;
    }

    Rfd = pfd[0];
    Wfd = pfd[1];

    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    pid_t forkPid = fork();
    if (forkPid < 0) {
        TError error(EError::Unknown, errno, "fork()");
        L_ERR() << "Can't spawn child: " << error << std::endl;
        return error;
    } else if (forkPid == 0) {
        SetProcessName("portod-spawn-p");

        char stack[8192];

        (void)setsid();

        TError error = Env->Ns.Attach();
        if (error) {
            L_ERR() << "Can't spawn child: " << error << std::endl;
            ReportPid(-1);
            Abort(error);
        }

        int cloneFlags = SIGCHLD;
        if (Env->Isolate) {
            cloneFlags |= CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC;
        } else {
            Env->NetCfg.Share = true;
            Env->NetCfg.Host.clear();
            Env->NetCfg.MacVlan.clear();
        }

        if (Env->Hostname != "")
            cloneFlags |= CLONE_NEWUTS;

        if (!Env->NetCfg.Share)
            cloneFlags |= CLONE_NEWNET;

        int ret = pipe2(syncfd, O_CLOEXEC);
        if (ret) {
            TError error(EError::Unknown, errno, "pipe2(pdf)");
            L_ERR() << "Can't create sync pipe for child: " << error << std::endl;
            ReportPid(-1);
            Abort(error);
        }

        WaitParentRfd = syncfd[0];
        WaitParentWfd = syncfd[1];

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);
        if (clonePid < 0) {
            TError error(EError::Unknown, errno, "clone()");
            L_ERR() << "Can't spawn child: " << error << std::endl;
            ReportPid(-1);
            Abort(error);
        }

        if (config().network().enabled()) {
            error = IsolateNet(clonePid);
            if (error) {
                (void)kill(clonePid, SIGKILL);
                L_ERR() << "Can't spawn child: " << error << std::endl;
                ReportPid(-1);
                Abort(error);
            }
        }

        int result = 0;
        if (write(WaitParentWfd, &result, sizeof(result)) != sizeof(result)) {
            TError error(EError::Unknown, "Partial write to child sync pipe");
            (void)kill(clonePid, SIGKILL);
            L_ERR() << "Can't spawn child: " << error << std::endl;
            ReportPid(-1);
            Abort(error);
        }

        ReportPid(clonePid);
        exit(EXIT_SUCCESS);
    }
    (void)waitpid(forkPid, NULL, 0);

    close(Wfd);
    int n = read(Rfd, &Pid, sizeof(Pid));
    if (n <= 0) {
        TError error(EError::Unknown, errno, "read(Rfd)");
        L_ERR() << "Can't read pid from the child: " << error << std::endl;
        return error;
    }

    TError error = TError::Deserialize(Rfd);
    close(Rfd);
    if (error) {
        Pid = 0;
        ExitStatus = -1;

        return error;
    }

    State = Started;
    return TError::Success();
}

int TTask::GetPid() const {
    return Pid;
}

bool TTask::IsRunning() const {
    return State == Started;
}

int TTask::GetExitStatus() const {
    return ExitStatus;
}

void TTask::DeliverExitStatus(int status) {
    LeafCgroups.clear();
    ExitStatus = status;
    State = Stopped;
}

TError TTask::Kill(int signal) const {
    if (!Pid)
        throw "Tried to kill invalid process!";

    L() << "kill " << signal << " " << Pid << std::endl;

    int ret = kill(Pid, signal);
    if (ret != 0)
        return TError(EError::Unknown, errno, "kill(" + std::to_string(Pid) + ")");

    return TError::Success();
}

std::string TTask::GetStdout(size_t limit) const {
    if (Env->StdoutPath.GetType() != EFileType::Regular)
        return "";

    string s;
    TFile f(Env->StdoutPath);
    TError error(f.LastStrings(limit, s));
    if (error)
        L_ERR() << "Can't read container stdout: " << error << std::endl;
    return s;
}

std::string TTask::GetStderr(size_t limit) const {
    if (Env->StderrPath.GetType() != EFileType::Regular)
        return "";

    string s;
    TFile f(Env->StderrPath);
    TError error(f.LastStrings(limit, s));
    if (error)
        L_ERR() << "Can't read container stderr: " << error << std::endl;
    return s;
}

TError TTask::Restore(int pid_) {
    ExitStatus = 0;
    Pid = pid_;
    State = Started;

    bool running = kill(Pid, 0) == 0;

    // There are several possibilities here:
    // 1. We died and loop reaped container, so it will deliver
    // exit_status later;
    // 2. In previous session we died right after we reaped exit_status
    // but didn't save it to persistent store.
    // 3. We died in consistent dead state.
    // 4. We died in consistent stopped state.
    //
    // Thus, we need to be in Started state so we can possibly receive
    // exit_status from (1); if it was really case (2) we will indicate
    // error when user tries to get task state in Reap() from waitpit().
    //
    // For 3/4 case we rely on the saved state.
    //
    // Moreover, if task didn't die, but we are restoring, it can go
    // away under us any time, so don't fail if we can't recover
    // something.

    Env->StdinPath = "";
    Env->StdoutPath = Env->Cwd + "/stdout";
    Env->StderrPath = Env->Cwd + "/stderr";

    if (running) {
        TPath stdinLink("/proc/" + std::to_string(Pid) + "/fd/0");
        TError error = stdinLink.ReadLink(Env->StdinPath);
        if (error)
            L_WRN() << "Can't restore stdin: " << error << std::endl;

        TPath stdoutLink("/proc/" + std::to_string(Pid) + "/fd/1");
        error = stdoutLink.ReadLink(Env->StdoutPath);
        if (error)
            L_WRN() << "Can't restore stdout: " << error << std::endl;

        TPath stderrLink("/proc/" + std::to_string(Pid) + "/fd/2");
        error = stderrLink.ReadLink(Env->StderrPath);
        if (error)
            L_WRN() << "Can't restore stderr: " << error << std::endl;

        error = FixCgroups();
        if (error)
            L_WRN() << "Can't fx cgroups: " << error << std::endl;
    }

    return TError::Success();
}

TError TTask::FixCgroups() const {
    map<string, string> cgmap;
    TError error = GetTaskCgroups(Pid, cgmap);
    if (error)
        return error;

    for (auto pair : cgmap) {
        auto subsys = TSubsystem::Get(pair.first);
        auto &path = pair.second;

        if (!subsys || LeafCgroups.find(subsys) == LeafCgroups.end()) {
            if (pair.first.find(',') != std::string::npos)
                continue;
            if (pair.first == "net_cls" && !config().network().enabled()) {
                if (path == "/")
                    continue;

                L_WRN() << "No network, disabled " << subsys->GetName() << ":" << path << std::endl;

                auto cg = subsys->GetRootCgroup();
                error = cg->Attach(Pid);
                if (error)
                    L_ERR() << "Can't reattach to root: " << error << std::endl;
                continue;
            }

            error = TError(EError::Unknown, "Task belongs to unknown subsystem " + pair.first);
            L_WRN() << "Skip " << pair.first << ": " << error << std::endl;
            continue;
        }

        auto cg = LeafCgroups.at(subsys);
        if (cg->Relpath() != path) {
            L_WRN() << "Fixed invalid task subsystem for " << subsys->GetName() << ":" << path << std::endl;

            error = cg->Attach(Pid);
            if (error)
                L_ERR() << "Can't fix: " << error << std::endl;
        }
    }

    return TError::Success();
}

TError TTask::RotateFile(const TPath &path) const {
    if (!path.ToString().length())
        return TError::Success();

    if (path.GetType() != EFileType::Regular)
        return TError::Success();

    TFile f(path);
    if (f.GetSize() > config().container().max_log_size()) {
        TError error = f.Truncate(0);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::Rotate() const {
    TError error;

    error = RotateFile(Env->StdoutPath);
    if (error)
        return error;

    error = RotateFile(Env->StderrPath);
    if (error)
        return error;

    return TError::Success();
}

TError TaskGetLastCap() {
    TFile f("/proc/sys/kernel/cap_last_cap");
    return f.AsInt(lastCap);
}
