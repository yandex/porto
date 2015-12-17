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
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
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

const char** TTaskEnv::GetEnvp() const {
    auto envp = new const char* [Environ.size() + 1];
    for (size_t i = 0; i < Environ.size(); i++)
        envp[i] = strdup(Environ[i].c_str());
    envp[Environ.size()] = NULL;

    return envp;
}

bool TTaskEnv::EnvHasKey(const std::string &key) {
    for (auto str : Environ) {
        std::string envKey;

        std::vector<std::string> tok;
        TError error = SplitString(str, '=', tok, 2);
        if (error)
            envKey = str;
        else
            envKey = tok[0];

        if (key == envKey)
            return true;
    }

    return false;
}

// TTask
//

TTask::TTask(std::unique_ptr<TTaskEnv> &env) : Env(std::move(env)) {}

TTask::TTask(pid_t pid) : Pid(pid) {}

void TTask::ReportPid(pid_t pid) const {
    TError error = Env->Sock.SendPid(pid);
    if (error) {
        L_ERR() << error << std::endl;
        Abort(error);
    }
    Env->ReportStage++;
}

void TTask::Abort(const TError &error) const {
    TError error2;

    /*
     * stage0: RecvPid WPid
     * stage1: RecvPid VPid
     * stage2: RecvError
     */
    L() << "abort due to " << error << std::endl;

    for (int stage = Env->ReportStage; stage < 2; stage++) {
        error2 = Env->Sock.SendPid(getpid());
        if (error2)
            L_ERR() << error2 << std::endl;
    }

    error2 = Env->Sock.SendError(error);
    if (error2)
        L_ERR() << error2 << std::endl;

    exit(EXIT_FAILURE);
}

static int ChildFn(void *arg) {
    SetProcessName("portod-spawn-c");
    TTask *task = static_cast<TTask*>(arg);
    task->StartChild();
    return EXIT_FAILURE;
}

TError TTask::ChildOpenStdFile(const TPath &path, int expected) {
    int ret = open(path.ToString().c_str(), O_CREAT | O_WRONLY | O_APPEND, 0660);
    if (ret < 0)
        return TError(EError::InvalidValue, errno,
                      "open(" + path.ToString() + ") -> " +
                      std::to_string(expected));

    if (ret != expected) {
        if (dup2(ret, expected) < 0) {
            close(ret);
            return TError(EError::Unknown, errno,
                    "dup2(" + std::to_string(ret) + ", " + std::to_string(expected) + ")");
        }
        close(ret);
        ret = expected;
    }

    if (path.IsRegular()) {
        ret = fchown(ret, Env->Cred.Uid, Env->Cred.Gid);
        if (ret < 0)
            return TError(EError::Unknown, errno,
                    "fchown(" + path.ToString() + ") -> " +
                    std::to_string(expected));
    }

    return TError::Success();
}

TError TTask::ReopenStdio(bool open_default) {
    TError error;

    if (open_default == Env->DefaultStdin) {
        int ret = open(Env->StdinPath.ToString().c_str(), O_CREAT | O_RDONLY, 0660);
        if (ret < 0)
            return TError(EError::Unknown, errno, "open(" + Env->StdinPath.ToString() + ") -> 0");
        if (ret != 0) {
            if (dup2(ret, 0) < 0) {
                close(ret);
                return TError(EError::Unknown, errno, "dup2(" + std::to_string(ret) + ", 0)");
            }
            close(ret);
            ret = 0;
        }
    }

    if (open_default == Env->DefaultStdout) {
        error = ChildOpenStdFile(Env->StdoutPath, 1);
        if (error)
            return error;
    }

    if (open_default == Env->DefaultStderr) {
        error = ChildOpenStdFile(Env->StderrPath, 2);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildApplyCapabilities() {
    uint64_t effective, permitted, inheritable;

    if (!Env->Cred.IsRootUser())
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

    if (setgroups(Env->Cred.Groups.size(), Env->Cred.Groups.data()) < 0)
        return TError(EError::Unknown, errno, "setgroups()");

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
    auto envp = Env->GetEnvp();

    if (Env->Command.empty()) {
        const char *args[] = {
            "portoinit",
            "--container",
            Env->Container.c_str(),
            NULL,
        };
        SetDieOnParentExit(0);
        fexecve(Env->PortoInitFd.GetFd(), (char *const *)args, (char *const *)envp);
        return TError(EError::InvalidValue, errno, "fexecve(" +
                      std::to_string(Env->PortoInitFd.GetFd()) +  ", portoinit)");
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

    if (config().log().verbose()) {
        L() << "command=" << Env->Command << std::endl;
        for (unsigned i = 0; result.we_wordv[i]; i++)
            L() << "argv[" << i << "]=" << result.we_wordv[i] << std::endl;
        for (unsigned i = 0; envp[i]; i++)
            L() << "environ[" << i << "]=" << envp[i] << std::endl;
    }
    SetDieOnParentExit(0);
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, (char *const *)envp);

    return TError(EError::InvalidValue, errno, string("execvpe(") + result.we_wordv[0] + ", " + std::to_string(result.we_wordc) + ", " + std::to_string(Env->Environ.size()) + ")");
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
    for (auto &bindMap : Env->BindMap) {
        TPath src, dest;

        if (bindMap.Source.IsAbsolute())
            src = bindMap.Source;
        else
            src = Env->ParentCwd / bindMap.Source;

        if (bindMap.Dest.IsAbsolute())
            dest = Env->Root / bindMap.Dest;
        else
            dest = Env->Root / Env->Cwd / bindMap.Dest;

        if (!StringStartsWith(dest.RealPath().ToString(), Env->Root.ToString()))
            return TError(EError::InvalidValue, "Container bind mount "
                          + src.ToString() + " resolves to root "
                          + dest.RealPath().ToString()
                          + " (" + Env->Root.ToString() + ")");

        TMount mnt(src, dest, "none", {});

        TError error;
        if (src.IsDirectory())
            error = mnt.BindDir(bindMap.Rdonly);
        else
            error = mnt.BindFile(bindMap.Rdonly);
        if (error)
            return error;

        // drop nosuid,noexec,nodev from volumes
        if (Env->NewMountNs) {
            error = dest.Remount(MS_REMOUNT | MS_BIND |
                                 (bindMap.Rdonly ? MS_RDONLY : 0));
            if (error)
                return error;
        }
    }

    return TError::Success();
}

TError TTask::ChildRemountRootRo() {
    if (!Env->RootRdOnly || Env->LoopDev >= 0)
        return TError::Success();

    // remount everything except binds to ro
    std::vector<std::shared_ptr<TMount>> snapshot;
    TError error = TMount::Snapshot(snapshot);
    if (error)
        return error;

    for (auto mnt : snapshot) {
        TPath path = Env->Root.InnerPath(mnt->GetMountpoint());
        if (path.IsEmpty())
            continue;

        bool skip = false;
        for (auto &bindMap : Env->BindMap) {
            TPath dest = bindMap.Dest;

            if (dest.NormalPath() == path.NormalPath()) {
                skip = true;
                break;
            }
        }
        if (skip)
            continue;

        error = mnt->GetMountpoint().Remount(MS_REMOUNT | MS_BIND | MS_RDONLY);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildMountRootFs() {
    TError error;

    if (Env->Root.IsRoot())
        return TError::Success();

    if (Env->LoopDev >= 0)
        error = Env->Root.Mount("/dev/loop" + std::to_string(Env->LoopDev),
                                "ext4", Env->RootRdOnly ? MS_RDONLY : 0, {});
    else
        error = Env->Root.Bind(Env->Root, 0);
    if (error)
        return error;

    struct {
        std::string target;
        std::string type;
        unsigned long flags;
        std::vector<std::string> opts;
    } mounts[] = {
        { "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME,
            { "mode=755", "size=32m" } },
        { "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
            { "newinstance", "ptmxmode=0666", "mode=620" ,"gid=5" }},
        { "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, {}},
        { "/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_STRICTATIME,
            { "mode=755", "size=32m" }},
        { "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, {}},
    };

    for (auto &m : mounts) {
        TPath target = Env->Root + m.target;
        error = target.MkdirAll(0755);
        if (!error)
            error = target.Mount(m.type, m.type, m.flags, m.opts);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        mode_t mode;
    } dirs[] = {
        { "/run/lock",  01777 },
        { "/run/shm",   01777 },
    };

    for (auto &d : dirs) {
        error = (Env->Root + d.path).Mkdir(d.mode);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        mode_t mode;
        dev_t dev;
    } nodes[] = {
        { "/dev/null",    0666 | S_IFCHR, MKDEV(1, 3) },
        { "/dev/zero",    0666 | S_IFCHR, MKDEV(1, 5) },
        { "/dev/full",    0666 | S_IFCHR, MKDEV(1, 7) },
        { "/dev/random",  0666 | S_IFCHR, MKDEV(1, 8) },
        { "/dev/urandom", 0666 | S_IFCHR, MKDEV(1, 9) },
        { "/dev/tty",     0666 | S_IFCHR, MKDEV(5, 0) },
        { "/dev/console", 0600 | S_IFCHR, MKDEV(1, 3) }, /* to /dev/null */
    };

    for (auto &n : nodes) {
        error = (Env->Root + n.path).Mknod(n.mode, n.dev);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        const std::string target;
    } symlinks[] = {
        { "/dev/ptmx", "pts/ptmx" },
        { "/dev/fd", "/proc/self/fd" },
        { "/dev/shm", "../run/shm" },
    };

    for (auto &s : symlinks) {
        error = (Env->Root + s.path).Symlink(s.target);
        if (error)
            return error;
    }

    std::vector<std::string> proc_ro = {
        "/proc/sysrq-trigger",
        "/proc/irq",
        "/proc/bus",
    };

    if (!Env->Cred.IsRootUser())
        proc_ro.push_back("/proc/sys");

    for (auto &p : proc_ro) {
        TPath path = Env->Root + p;
        error = path.Bind(path, MS_RDONLY);
        if (error)
            return error;
    }

    TPath proc_kcore = Env->Root + "/proc/kcore";
    error = proc_kcore.Bind(Env->Root + "/dev/null", MS_RDONLY);
    if (error)
        return error;

    if (Env->BindDns) {
        error = ChildBindDns();
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTask::ChildIsolateFs() {

    if (Env->Root.IsRoot())
        return TError::Success();

    TError error = Env->Root.PivotRoot();
    if (error) {
        L_WRN() << "Can't pivot root, roll back to chroot: " << error << std::endl;

        error = Env->Root.Chroot();
        if (error)
            return error;
    }

    // Allow suid binaries and device nodes at container root.
    error = TPath("/").Remount(MS_REMOUNT | MS_BIND |
                               (Env->RootRdOnly ? MS_RDONLY : 0));
    if (error) {
        L_ERR() << "Can't remount / as suid and dev:" << error << std::endl;
        return error;
    }

    TPath newRoot("/");
    return newRoot.Chdir();
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
    if (Env->Hostname == "")
        return TError::Success();

    if (Env->SetEtcHostname) {
        TFile f("/etc/hostname");
        if (f.Exists()) {
            string host = Env->Hostname + "\n";
            TError error = f.WriteStringNoAppend(host);
            if (error)
                return TError(EError::Unknown, error.GetErrno(), "write(/etc/hostname)");
        }
    }

    if (sethostname(Env->Hostname.c_str(), Env->Hostname.length()) < 0)
        return TError(EError::Unknown, errno, "sethostname()");

    return TError::Success();
}

TError TTask::ConfigureChild() {
    TError error;

    /* Die together with waiter */
    if (Env->TripleFork)
        SetDieOnParentExit(SIGKILL);

    ResetAllSignalHandlers();
    error = ChildApplyLimits();
    if (error)
        return error;

    if (setsid() < 0)
        return TError(EError::Unknown, errno, "setsid()");

    umask(0);

    if (Env->NewMountNs) {
        // Remount to slave to receive propogations from parent namespace
        error = TPath("/").Remount(MS_SLAVE | MS_REC);
        if (error)
            return error;
    }

    if (Env->Isolate) {
        // remount proc so PID namespace works
        TPath tmpProc("/proc");
        error = tmpProc.UmountAll();
        if (error)
            return error;
        error = tmpProc.Mount("proc", "proc",
                              MS_NOEXEC | MS_NOSUID | MS_NODEV, {});
        if (error)
            return error;
    }

    error = ChildMountRootFs();
    if (error)
        return error;

    error = ChildBindDirectores();
    if (error)
        return error;

    error = ChildRemountRootRo();
    if (error)
        return error;

    error = ChildIsolateFs();
    if (error)
        return error;

    error = ChildSetHostname();
    if (error)
        return error;

    error = Env->Cwd.Chdir();
    if (error)
        return error;

    if (Env->NewMountNs) {
        // Make all shared: subcontainers will get propgation from us
        error = TPath("/").Remount(MS_SHARED | MS_REC);
        if (error)
            return error;
    }

    error = ChildApplyCapabilities();
    if (error)
        return error;

    if (Env->QuadroFork) {
        Pid = fork();
        if (Pid < 0)
            return TError(EError::Unknown, errno, "fork()");

        if (Pid) {
            auto fd = Env->PortoInitFd.GetFd();
            auto pid_ = std::to_string(Pid);
            const char * argv[] = {
                "portoinit",
                "--container",
                Env->Container.c_str(),
                "--wait",
                pid_.c_str(),
                NULL,
            };
            auto envp = Env->GetEnvp();

            CloseFds(-1, { fd } );
            fexecve(fd, (char *const *)argv, (char *const *)envp);
            return TError(EError::Unknown, errno, "fexecve()");
        } else {
            Pid = getpid();
            error = Env->Sock2.SendPid(Pid);
            if (error)
                return error;
            error = Env->Sock2.RecvZero();
            if (error)
                return error;
            /* Parent forwards VPid */
            Env->ReportStage++;
        }
    }

    error = ChildDropPriveleges();
    if (error)
        return error;

    error = ReopenStdio(false);
    if (error)
        return error;

    return TError::Success();
}

void TTask::StartChild() {
    TError error;

    /* WPid reported by parent */
    Env->ReportStage++;

    /* Wait for report WPid in parent */
    error = Env->Sock.RecvZero();
    if (error)
        Abort(error);

    /* Report VPid in pid namespace we're enter */
    if (!Env->Isolate)
        ReportPid(getpid());
    else if (!Env->QuadroFork)
        Env->ReportStage++;

    /* Apply configuration */
    error = ConfigureChild();
    if (error)
        Abort(error);

    /* Wait for Wakeup */
    error = Env->Sock.RecvZero();
    if (error)
        Abort(error);

    error = ChildExec();
    Abort(error);
}

TError TTask::Start() {
    TUnixSocket waiterSock;
    int status;
    TError error;

    Pid = VPid = WPid = 0;
    ExitStatus = 0;

    error = TUnixSocket::SocketPair(Env->MasterSock, Env->Sock);
    if (error)
        return error;

    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    pid_t forkPid = fork();
    if (forkPid < 0) {
        TError error(EError::Unknown, errno, "fork()");
        L() << "Can't spawn child: " << error << std::endl;
        return error;
    } else if (forkPid == 0) {
        TError error;

        SetDieOnParentExit(SIGKILL);
        SetProcessName("portod-spawn-p");

        char stack[8192];

        (void)setsid();

        // move to target cgroups
        for (auto cg : Env->LeafCgroups) {
            error = cg.second->Attach(getpid());
            if (error)
                Abort(error);
        }

        /* Default stdio/stderr are in host mount namespace */
        error = ReopenStdio(true);
        if (error)
            Abort(error);

        /* Enter parent namespaces */
        error = Env->ParentNs.Enter();
        if (error)
            Abort(error);

        if (Env->TripleFork) {
            /*
             * Enter into pid-namespace. fork() hangs in libc if child pid
             * collide with parent pid outside. vfork() has no such problem.
             */
            forkPid = vfork();
            if (forkPid < 0)
                Abort(TError(EError::Unknown, errno, "fork()"));

            if (forkPid)
                _exit(EXIT_SUCCESS);
        }

        if (Env->QuadroFork) {
            error = TUnixSocket::SocketPair(waiterSock, Env->Sock2);
            if (error)
                Abort(error);
        }

        int cloneFlags = SIGCHLD;
        if (Env->Isolate)
            cloneFlags |= CLONE_NEWPID | CLONE_NEWIPC;

        if (Env->NewMountNs)
            cloneFlags |= CLONE_NEWNS;

        /* Create UTS namspace if hostname is changed or isolate=true */
        if (Env->Isolate || Env->Hostname != "")
            cloneFlags |= CLONE_NEWUTS;

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);

        if (clonePid < 0) {
            TError error(errno == ENOMEM ?
                         EError::ResourceNotAvailable :
                         EError::Unknown, errno, "clone()");
            Abort(error);
        }

        /* Report WPid in host pid namespace */
        if (Env->TripleFork)
            ReportPid(GetTid());
        else
            ReportPid(clonePid);

        /* Report VPid in parent pid namespace for new pid-ns */
        if (Env->Isolate && !Env->QuadroFork)
            ReportPid(clonePid);

        /* WPid reported, wakeup child */
        error = Env->MasterSock.SendZero();
        if (error)
            Abort(error);

        /* ChildCallback() reports VPid here if !Env->Isolate */
        if (!Env->Isolate && !Env->QuadroFork)
            Env->ReportStage++;

        /*
         * QuadroFork waiter receives application VPid from init
         * task and forwards it into host.
         */
        if (Env->QuadroFork) {
            pid_t appPid, appVPid;

            error = waiterSock.RecvPid(appPid, appVPid);
            if (error)
                Abort(error);
            /* Forward VPid */
            ReportPid(appPid);
            error = waiterSock.SendZero();
            if (error)
                Abort(error);
        }

        if (Env->TripleFork) {
            auto fd = Env->PortoInitFd.GetFd();
            auto pid = std::to_string(clonePid);
            const char * argv[] = {
                "portoinit",
                "--container",
                Env->Container.c_str(),
                "--wait",
                pid.c_str(),
                NULL,
            };
            auto envp = Env->GetEnvp();

            CloseFds(-1, { fd } );
            fexecve(fd, (char *const *)argv, (char *const *)envp);
            kill(clonePid, SIGKILL);
            _exit(EXIT_FAILURE);
        }

        _exit(EXIT_SUCCESS);
    }

    Env->Sock.Close();

    status = 0;
    int forkResult = waitpid(forkPid, &status, 0);
    if (forkResult < 0)
        (void)kill(forkPid, SIGKILL);

    error = Env->MasterSock.RecvPid(WPid, VPid);
    if (error)
        return error;

    error = Env->MasterSock.RecvPid(Pid, VPid);
    if (error)
        return error;

    if (status)
        return TError(EError::Unknown, "Start failed, status " + std::to_string(status));

    return TError::Success();
}

TError TTask::Wakeup() {
    TError error, wakeup_error;

    wakeup_error = Env->MasterSock.SendZero();

    error = Env->MasterSock.RecvError();

    if (!error && wakeup_error)
        error = wakeup_error;

    ClearEnv();

    if (error) {
        if (Pid > 0) {
            (void)kill(Pid, SIGKILL);
            L_ACT() << "Kill partly constructed container " << Pid << ": " << strerror(errno) << std::endl;
        }
        Pid = VPid = WPid = 0;
        ExitStatus = -1;
    } else
        State = Started;

    return error;
}

pid_t TTask::GetPid() const {
    return Pid;
}

pid_t TTask::GetWPid() const {
    return WPid;
}

std::vector<int> TTask::GetPids() const {
    return {Pid, VPid, WPid};
}

pid_t TTask::GetPidFor(pid_t pid) const {
    if (InPidNamespace(pid, getpid()))
        return Pid;
    if (WPid != Pid && InPidNamespace(pid, WPid))
        return VPid;
    if (InPidNamespace(pid, Pid))
        return 1;
    return 0;
}

bool TTask::IsRunning() const {
    return State == Started;
}

int TTask::GetExitStatus() const {
    return ExitStatus;
}

void TTask::Exit(int status) {
    ExitStatus = status;
    State = Stopped;
}

TError TTask::Kill(int signal) const {
    if (!Pid)
        throw "Tried to kill invalid process!";

    L_ACT() << "kill " << signal << " " << Pid << std::endl;

    int ret = kill(Pid, signal);
    if (ret != 0)
        return TError(EError::Unknown, errno, "kill(" + std::to_string(Pid) + ")");

    return TError::Success();
}

bool TTask::IsZombie() const {
    TFile f("/proc/" + std::to_string(WPid) + "/status");

    std::vector<std::string> lines;
    TError err = f.AsLines(lines);
    if (err)
        return false;

    for (auto &l : lines)
        if (l.compare(0, 7, "State:\t") == 0)
            return l.substr(7, 1) == "Z";

    return false;
}

bool TTask::HasCorrectParent() {
    pid_t ppid;
    TError error = GetTaskParent(WPid, ppid);
    if (error) {
        L() << "Can't get ppid of restored task: " << error << std::endl;
        return false;
    }

    if (ppid != getppid()) {
        L() << "Invalid ppid of restored task: " << ppid << " != " << getppid() << std::endl;
        return false;
    }

    return true;
}

bool TTask::HasCorrectFreezer() {
    // if task belongs to different freezer cgroup we don't
    // restore it since pids may have wrapped or previous kvs state
    // is too old
    map<string, string> cgmap;
    TError error = GetTaskCgroups(WPid, cgmap);
    if (error) {
        L() << "Can't read " << WPid << " cgroups of restored task: " << error << std::endl;
        return false;
    } else {
        auto cg = Env->LeafCgroups.at(freezerSubsystem);
        if (cg && cg->Relpath().ToString() != cgmap["freezer"]) {
            // if at this point task is zombie we don't have any cgroup info
            if (IsZombie())
                return true;

            L_WRN() << "Unexpected freezer cgroup of restored task  " << WPid << ": " << cg->Path() << " != " << cgmap["freezer"] << std::endl;
            Pid = VPid = WPid = 0;
            State = Stopped;
            return false;
        }
    }

    return true;
}

void TTask::Restore(std::vector<int> pids) {
    ExitStatus = 0;
    Pid = pids[0];
    VPid = pids[1];
    WPid = pids[2];
    State = Started;
}

TError TTask::SyncTaskCgroups(pid_t pid) const {
    if (IsZombie())
        return TError::Success();

    map<string, string> cgmap;
    TError error = GetTaskCgroups(pid, cgmap);
    if (error)
        return error;

    for (auto pair : cgmap) {
        auto subsys = TSubsystem::Get(pair.first);
        auto &path = pair.second;

        if (!subsys || Env->LeafCgroups.find(subsys) == Env->LeafCgroups.end()) {
            if (pair.first.find(',') != std::string::npos)
                continue;
            if (pair.first == "net_cls") {
                if (path == "/")
                    continue;

                L_WRN() << "No network, disabled " << subsys->GetName() << ":" << path << std::endl;

                auto cg = subsys->GetRootCgroup();
                error = cg->Attach(pid);
                if (error)
                    L_ERR() << "Can't reattach to root: " << error << std::endl;
                continue;
            }

            error = TError(EError::Unknown, "Task belongs to unknown subsystem " + pair.first);
            L_WRN() << "Skip " << pair.first << ": " << error << std::endl;
            continue;
        }

        auto cg = Env->LeafCgroups.at(subsys);
        if (cg && cg->Relpath().ToString() != path) {
            L_WRN() << "Fixed invalid task subsystem for " << subsys->GetName() << ":" << path << std::endl;

            error = cg->Attach(pid);
            if (error)
                L_ERR() << "Can't fix: " << error << std::endl;
        }
    }

    return TError::Success();
}

TError TTask::SyncCgroupsWithFreezer() const {
    auto freezer = Env->LeafCgroups.at(freezerSubsystem);
    std::vector<pid_t> tasks;

    TError error = freezer->GetTasks(tasks);
    if (error)
        return error;

    if (std::find(tasks.begin(), tasks.end(), Pid) == tasks.end())
        return TError(EError::Unknown, "Pid " + std::to_string(Pid) + " not in freezer");

    if (Pid != WPid && std::find(tasks.begin(), tasks.end(), WPid) == tasks.end())
        return TError(EError::Unknown, "WPid " + std::to_string(Pid) + " not in freezer");

    for (pid_t pid : tasks) {
        error = SyncTaskCgroups(pid);
        if (error)
            L_WRN() << "Cannot sync cgroups of " << pid << " : " << error << std::endl;
    }

    return TError::Success();
}

void TTask::ClearEnv() {
    Env = nullptr;
}

TError TaskGetLastCap() {
    TFile f("/proc/sys/kernel/cap_last_cap");
    return f.AsInt(lastCap);
}

TError TTask::DumpProcFsFile(const std::string &filename) {
    TFile f("/proc/" + std::to_string(Pid) + "/" + filename);
    std::vector<std::string> lines;
    TError err = f.AsLines(lines);
    if (err) {
        L_ERR() << "Cannot read proc/" << filename << " for pid " << Pid << std::endl;
        return err;
    }
    L() << "Dump proc/" << filename << " status for pid " << Pid << std::endl;
    for (const auto& line : lines)
        L() << line << std::endl;
    L() << "----" << std::endl;

    return TError::Success();
}

void TTask::DumpDebugInfo() {
    DumpProcFsFile("status");
    DumpProcFsFile("stack");
}

TTask::~TTask() {
}
