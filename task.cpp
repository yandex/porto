#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "porto.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/pwd.hpp"
#include "util/netlink.hpp"

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
}

using std::stringstream;
using std::string;
using std::vector;
using std::map;

// TTaskEnv

TError TTaskEnv::Prepare() {
    if (Command.empty())
        return TError::Success();

    EnvVec.push_back("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    TError error = SplitEscapedString(Environ, ';', EnvVec);
    if (error)
        return error;
    EnvVec.push_back("HOME=" + Cwd.ToString());
    EnvVec.push_back("USER=" + User);

    TUser u(User);
    error = u.Load();
    if (error)
        return error;

    Uid = u.GetId();

    TGroup g(Group);
    error = g.Load();
    if (error)
        return error;

    Gid = g.GetId();

    return TError::Success();
}

const char** TTaskEnv::GetEnvp() const {
    auto envp = new const char* [EnvVec.size() + 1];
    for (size_t i = 0; i < EnvVec.size(); i++)
        envp[i] = EnvVec[i].c_str();
    envp[EnvVec.size()] = NULL;

    return envp;
}

// TTask
int TTask::CloseAllFds(int except) const {
    for (int i = 0; i < getdtablesize(); i++)
        if (i != except)
            close(i);

    return except;
}

void TTask::Syslog(const string &s) const {
    openlog("portod", LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_ERR, "%s", s.c_str());
    closelog();
}

TTask::~TTask() {
    if (!Env)
        return;

    TFile out(Env->StdoutPath);
    if (Env->StdoutPath.GetType() != EFileType::Character) {
        TError e = out.Remove();
        TLogger::LogError(e, "Can't remove task stdout " + Env->StdoutPath.ToString());
    }

    if (Env->StderrPath.GetType() != EFileType::Character) {
        TFile err(Env->StderrPath);
        TError e = err.Remove();
        TLogger::LogError(e, "Can't remove task stderr " + Env->StderrPath.ToString());
    }
}

void TTask::ReportPid(int pid) const {
    if (write(Wfd, &pid, sizeof(pid)) != sizeof(pid)) {
        Syslog("partial write of pid: " + std::to_string(pid));
    }
}

void TTask::Abort(int result, const string &msg) const {
    if (write(Wfd, &result, sizeof(result)) != sizeof(result)) {
        Syslog("partial write of result: " + std::to_string(result));
    } else {
        if (write(Wfd, msg.data(), msg.length()) != (ssize_t)msg.length())
            Syslog("partial write of message: " + msg);
    }

    exit(EXIT_FAILURE);
}

void TTask::Abort(const TError &error, const std::string &msg) const {
    if (msg == "")
        Abort(error.GetErrno(), error.GetMsg());
    else
        Abort(error.GetErrno(), msg);
}

static int ChildFn(void *arg) {
    TTask *task = static_cast<TTask*>(arg);
    return task->ChildCallback();
}

void TTask::ChildOpenStdFile(const TPath &path, int expected) {
    int ret = open(path.ToString().c_str(), O_CREAT | O_WRONLY | O_APPEND, 0700);
    if (ret < 0)
        Abort(errno, "open(" + path.ToString() + ") -> " + std::to_string(expected));

    if (ret != expected)
        Abort(EINVAL, "open(" + path.ToString() + ") -> " +
              std::to_string(expected) + ": unexpected fd");

    ret = fchown(ret, Env->Uid, Env->Gid);
    if (ret < 0)
        Abort(errno, "fchown(" + path.ToString() + ") -> " +
              std::to_string(expected));
}

void TTask::ChildReopenStdio() {
    Wfd = CloseAllFds(Wfd);
    if (Wfd < 0) {
        Syslog(string("close fds: ") + strerror(errno));
        /* there is no way of telling parent that we failed (because we
         * screwed up fds), so exit with some eye catching error code */
        exit(0xAA);
    }

    int ret = open(Env->StdinPath.ToString().c_str(), O_CREAT | O_RDONLY, 0700);
    if (ret < 0)
        Abort(errno, "open(0)");

    if (ret != 0)
        Abort(EINVAL, "open(0): unexpected fd");

    ChildOpenStdFile(Env->StdoutPath, 1);
    ChildOpenStdFile(Env->StderrPath, 2);
}

void TTask::ChildDropPriveleges() {
    if (Env->Uid || Env->Gid) {
        if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) < 0)
            Abort(errno, "prctl(PR_SET_KEEPCAPS)");
    }

    if (setgid(Env->Gid) < 0)
        Abort(errno, "setgid()");

    if (initgroups(Env->User.c_str(), Env->Gid) < 0)
        Abort(errno, "initgroups()");

    if (setuid(Env->Uid) < 0)
        Abort(errno, "setuid()");
}

void TTask::ChildExec() {
    clearenv();

    for (auto &s : Env->EnvVec) {
        char *d = strdup(s.c_str());
        putenv(d);
    }

	wordexp_t result;

	int ret = wordexp(Env->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
    switch (ret) {
    case WRDE_BADCHAR:
        Abort(EINVAL, "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }");
    case WRDE_BADVAL:
        Abort(EINVAL, "wordexp(): undefined shell variable was referenced");
    case WRDE_CMDSUB:
        Abort(EINVAL, "wordexp(): command substitution is not supported");
    case WRDE_SYNTAX:
        Abort(EINVAL, "wordexp(): syntax error");
    default:
    case WRDE_NOSPACE:
        Abort(EINVAL, "wordexp(): error " + std::to_string(ret));
    case 0:
        break;
    }

    if (config().log().verbose()) {
        Syslog(Env->Command.c_str());
        for (unsigned i = 0; i < result.we_wordc; i++)
            Syslog(result.we_wordv[i]);
    }

    auto envp = Env->GetEnvp();
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, (char *const *)envp);

    Abort(errno, string("execvpe(") + result.we_wordv[0] + ")");
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
        TMount mnt(bindMap.Source, Env->Root + Env->Cwd + bindMap.Dest,
                   "none", {});

        TError error;
        if (bindMap.Source.GetType() == EFileType::Regular)
            error = mnt.BindFile(bindMap.Rdonly);
        else
            error = mnt.BindDir(bindMap.Rdonly);
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

TError TTask::RestrictProc() {
    vector<string> dirs = { "/proc/sys", "/proc/sysrq-trigger",
        "/proc/irq", "/proc/bus" };

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

    TMount dev("tmpfs", Env->Root + "/dev", "tmpfs", { "mode=755" });
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
        return TError(EError::Unknown, errno, "symlink(pts/ptmx)");

    return TError::Success();
}

TError TTask::ChildIsolateFs(bool priveleged) {
    if (Env->Root.ToString() != "/") {
        TMount root(Env->Root, Env->Root, "none", {});
        TError error = root.BindDir(false);
        if (error)
            return error;
    }

    TError error = ChildBindDirectores();
    if (error)
        return error;

    if (Env->Root.ToString() == "/")
        return TError::Success();

    unsigned long defaultFlags = MS_NOEXEC | MS_NOSUID | MS_NODEV;
    unsigned long sysfsFlags = defaultFlags;
    if (!priveleged)
        sysfsFlags |= MS_RDONLY;

    TMount sysfs("sysfs", Env->Root + "/sys", "sysfs", {});
    error = sysfs.MountDir(sysfsFlags);
    if (error)
        return error;

    TMount proc("proc", Env->Root + "/proc", "proc", {});
    error = proc.MountDir(defaultFlags);
    if (error)
        return error;

    if (!priveleged) {
        error = RestrictProc();
        if (error)
            return error;
    }

    error = ChildMountDev();
    if (error)
        return error;

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
    for (auto &dev : devices) {
        auto link = std::make_shared<TNlLink>(nl, dev);
        TError error = link->Up();
        if (error)
            return error;
    }

    return TError::Success();
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

    for (auto &mvlan : Env->NetCfg.MacVlan) {
        auto link = std::make_shared<TNlLink>(nl, "portomv0");

        (void)link->Remove();

        TError error = link->AddMacVlan(mvlan.Master, mvlan.Type, mvlan.Hw);
        if (error)
            return error;

        error = link->ChangeNs(mvlan.Name, childPid);
        if (error)
            return error;
    }

    return TError::Success();
}

void TTask::ChildApplyLimits() {
    for (auto pair : Env->Rlimit) {
        int ret = setrlimit(pair.first, &pair.second);
        if (ret < 0)
            Abort(errno, "setrlimit(" + std::to_string(pair.first) + ", " + std::to_string(pair.second.rlim_cur) + ":" + std::to_string(pair.second.rlim_max) + ")");
    }
}

void TTask::ChildSetHostname() {
    if (Env->Hostname == "")
        return;

    TFile f(Env->Root + "/etc/hostname");
    if (f.Exists()) {
        string host = Env->Hostname + "\n";
        TError error = f.WriteStringNoAppend(host);
        if (error)
            Abort(error, "write(/etc/hostname)");
    }

    if (sethostname(Env->Hostname.c_str(), Env->Hostname.length()) < 0)
        Abort(errno, "sethostname()");
}

int TTask::ChildCallback() {
    int ret;
    if (read(WaitParentRfd, &ret, sizeof(ret)) != sizeof(ret))
        Abort(errno, "partial read from child sync pipe");

    close(Rfd);
    ResetAllSignalHandlers();
    ChildApplyLimits();

    if (setsid() < 0)
        Abort(errno, "setsid()");

    umask(0);

    if (Env->Isolate) {
        // remount proc so PID namespace works
        TMount proc("proc", "/proc", "proc", {});
        if (proc.MountDir())
            Abort(errno, "remount procfs");
    }

    // move to target cgroups
    for (auto cg : LeafCgroups) {
        auto error = cg->Attach(getpid());
        if (error)
            Abort(error, "cgroup attach");
    }

    bool priveleged = Env->Gid == 0 || Env->Uid == 0;

    ChildReopenStdio();

    TError error = ChildIsolateFs(priveleged);
    if (error)
        Abort(error);

    if (!Env->NetCfg.Share) {
        error = EnableNet();
        if (error)
            Abort(error);
    }

    error = Env->Cwd.Chdir();
    if (error)
        Abort(error);

    ChildSetHostname();
    ChildDropPriveleges();
    ChildExec();

    return 0;
}

TError TTask::CreateCwd() {
    if (!Env->CreateCwd)
        return TError::Success();

    Cwd = std::make_shared<TFolder>(Env->Cwd, true);
    if (!Cwd->Exists()) {
        TError error = Cwd->Create(0755, true);
        if (error)
            return error;
    }
    return Env->Cwd.Chown(Env->User, Env->Group);
}

TError TTask::Start() {
    int ret;
    int pfd[2], syncfd[2];

    TError error = CreateCwd();
    if (error) {
        TLogger::LogError(error, "Can't create temporary cwd");
        return error;
    }

    ExitStatus = 0;

    ret = pipe2(pfd, O_CLOEXEC);
    if (ret) {
        TError error(EError::Unknown, errno, "pipe2(pdf)");
        TLogger::LogError(error, "Can't create communication pipe for child");
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
        TLogger::LogError(error, "Can't spawn child");
        return error;
    } else if (forkPid == 0) {
        char stack[8192];

        (void)setsid();

        TError error = Env->Ns.Attach();
        if (error) {
            TLogger::LogError(error, "Can't spawn child");
            ReportPid(-1);
            Abort(error);
        }

        int cloneFlags = SIGCHLD;
        if (Env->Isolate)
            cloneFlags |= CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC;

        if (Env->Hostname != "")
            cloneFlags |= CLONE_NEWUTS;

        if (!Env->NetCfg.Share)
            cloneFlags |= CLONE_NEWNET;

        int ret = pipe2(syncfd, O_CLOEXEC);
        if (ret) {
            TError error(EError::Unknown, errno, "pipe2(pdf)");
            TLogger::LogError(error, "Can't create sync pipe for child");
            ReportPid(-1);
            Abort(error);
        }

        WaitParentRfd = syncfd[0];
        WaitParentWfd = syncfd[1];

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);
        if (clonePid < 0) {
            TError error(EError::Unknown, errno, "clone()");
            TLogger::LogError(error, "Can't spawn child");
            ReportPid(-1);
            Abort(error);
        }

        ReportPid(clonePid);

        if (config().network().enabled()) {
            error = IsolateNet(clonePid);
            if (error) {
                TLogger::LogError(error, "Can't spawn child: " + error.GetMsg());
                ReportPid(-1);
                Abort(error);
            }
        }

        int result = 0;
        if (write(WaitParentWfd, &result, sizeof(result)) != sizeof(result)) {
            Syslog("partial write to child sync pipe");
        }

        exit(EXIT_SUCCESS);
    }
    (void)waitpid(forkPid, NULL, 0);

    close(Wfd);
    int n = read(Rfd, &Pid, sizeof(Pid));
    if (n <= 0) {
        TError error(EError::Unknown, errno, "read(Rfd)");
        TLogger::LogError(error, "Can't read pid from the child");
        return error;
    }

    n = read(Rfd, &ret, sizeof(ret));
    char buf[1024];
    string msg;
    ssize_t len = read(Rfd, buf, sizeof(buf));
    if (len > 0)
        msg = string(buf, len) + ": ";
    close(Rfd);

    if (n < 0) {
        Pid = 0;
        TError error(EError::Unknown, errno, "read(Rfd)");
        TLogger::LogError(error, "Can't read result from the child");
        return error;
    } else if (n == 0) {
        State = Started;
        return TError::Success();
    } else {
        Pid = 0;
        ExitStatus = -1;

        TError error(EError::Unknown, ret, msg);
        TLogger::LogError(error, "Child process couldn't start");
        return error;
    }
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

    TLogger::Log() << "kill " << signal << " " << Pid << std::endl;

    int ret = kill(Pid, signal);
    if (ret != 0)
        return TError(EError::Unknown, errno, "kill(" + std::to_string(Pid) + ")");

    return TError::Success();
}

std::string TTask::GetStdout(size_t limit) const {
    string s;
    TFile f(Env->StdoutPath);
    TError e(f.LastStrings(limit, s));
    TLogger::LogError(e, "Can't read container stdout");
    return s;
}

std::string TTask::GetStderr(size_t limit) const {
    string s;
    TFile f(Env->StderrPath);
    TError e(f.LastStrings(limit, s));
    TLogger::LogError(e, "Can't read container stderr");
    return s;
}

TError TTask::Restore(int pid_) {
    ExitStatus = 0;

    // There are to possibilities here:
    // 1. We died and loop reaped container, so it will deliver
    // exit_status later;
    // 2. In previous session we died right after we reaped exit_status
    // but didn't change persistent store.
    //
    // Thus, we need to be in Started state so we can possibly receive
    // exit_status from (1); if it was really case (2) we will indicate
    // error when user tries to get task state in Reap() from waitpit().
    //
    // Moreover, if task didn't die, but we are restoring, it can go
    // away under us any time, so don't fail if we can't recover
    // something.

    TPath stdinLink("/proc/" + std::to_string(pid_) + "/fd/0");
    TError error = stdinLink.ReadLink(Env->StdinPath);
    if (error)
        Env->StdinPath = "";
    TLogger::LogError(error, "Restore stdin");

    TPath stdoutLink("/proc/" + std::to_string(pid_) + "/fd/1");
    error = stdoutLink.ReadLink(Env->StdoutPath);
    if (error)
        Env->StdoutPath = Env->Cwd + "/stdout";
    TLogger::LogError(error, "Restore stdout");

    TPath stderrLink("/proc/" + std::to_string(pid_) + "/fd/2");
    error = stderrLink.ReadLink(Env->StderrPath);
    if (error)
        Env->StderrPath = Env->Cwd + "/stderr";
    TLogger::LogError(error, "Restore stderr");

    Pid = pid_;
    State = Started;

    error = ValidateCgroups();
    TLogger::LogError(error, "Can't validate cgroups");

    return TError::Success();
}

TError TTask::ValidateCgroups() const {
    map<string, string> cgmap;
    TError error = GetTaskCgroups(Pid, cgmap);
    if (error)
        return error;

    for (auto pair : cgmap) {
        auto &subsys = pair.first;
        auto &path = pair.second;

        bool valid = false;
        for (auto cg : LeafCgroups) {
            if (cg->Relpath() == path) {
                valid = true;
                break;
            }
        }

        if (!valid)
            return TError(EError::Unknown, "Task belongs to invalid subsystem " + subsys + ":" + path);
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
