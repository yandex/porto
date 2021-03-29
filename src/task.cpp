#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "task.hpp"
#include "container.hpp"
#include "device.hpp"
#include "config.hpp"
#include "network.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/netlink.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <fcntl.h>
#include <unistd.h>
#include <wordexp.h>
#include <grp.h>
#include <net/if.h>
#include <linux/sched.h>
#include <linux/capability.h>
}

std::list<std::string> IpcSysctls = {
    "fs.mqueue.queues_max",
    "fs.mqueue.msg_max",
    "fs.mqueue.msgsize_max",
    "fs.mqueue.msg_default",
    "fs.mqueue.msgsize_default",

    "kernel.shmmax",
    "kernel.shmall",
    "kernel.shmmni",
    "kernel.shm_rmid_forced",

    "kernel.msgmax",
    "kernel.msgmni",
    "kernel.msgmnb",

    "kernel.sem",
};

extern bool EnableCgroupNs;
extern bool EnableDockerMode;

void InitIpcSysctl() {
    for (const auto &key: IpcSysctls) {
        bool set = false;
        for (const auto &it: config().container().ipc_sysctl())
            set |= it.key() == key;
        std::string val;
        /* load default ipc sysctl from host config */
        if (!set && !GetSysctl(key, val)) {
            auto sysctl = config().mutable_container()->add_ipc_sysctl();
            sysctl->set_key(key);
            sysctl->set_val(val);
        }
    }
}

unsigned ProcBaseDirs;

void InitProcBaseDirs() {
    std::vector<std::string> dirs;
    TPath("/proc").ListSubdirs(dirs);
    for (auto &dir: dirs)
        if (!StringOnlyDigits(dir))
            ProcBaseDirs++;
    ProcBaseDirs += 2;
}

void TTaskEnv::ReportPid(pid_t pid) {
    TError error = Sock.SendPid(pid);
    if (error && error.Errno != ENOMEM) {
        L_ERR("{}", error);
        Abort(error);
    }
    ReportStage++;
}

void TTaskEnv::Abort(const TError &error) {
    TError error2;

    /*
     * stage0: RecvPid WPid
     * stage1: RecvPid VPid
     * stage2: RecvError
     */
    L("abort due to {}", error);

    for (int stage = ReportStage; stage < 2; stage++) {
        error2 = Sock.SendPid(GetPid());
        if (error2 && error2.Errno != ENOMEM)
            L_ERR("{}", error2);
    }

    error2 = Sock.SendError(error);
    if (error2 && error2.Errno != ENOMEM)
        L_ERR("{}", error2);

    _exit(EXIT_FAILURE);
}

static int ChildFn(void *arg) {
    TTaskEnv *task = static_cast<TTaskEnv*>(arg);
    task->StartChild();
    return EXIT_FAILURE;
}

TError TTaskEnv::OpenNamespaces(TContainer &ct) {
    TError error;

    auto target = &ct;
    while (target && !target->Task.Pid)
        target = target->Parent.get();

    if (!target)
        return OK;

    pid_t pid = target->Task.Pid;

    error = IpcFd.Open(pid, "ns/ipc");
    if (error)
        return error;

    error = UtsFd.Open(pid, "ns/uts");
    if (error)
        return error;

    if (NetFd.GetFd() < 0) {
        error = NetFd.Open(pid, "ns/net");
        if (error)
            return error;
    }

    error = PidFd.Open(pid, "ns/pid");
    if (error)
        return error;

    error = MntFd.Open(pid, "ns/mnt");
    if (error)
        return error;

    if (EnableCgroupNs) {
        error = CgFd.Open(pid, "ns/cgroup");
        if (error)
            return error;
    }

    error = RootFd.Open(pid, "root");
    if (error)
        return error;

    error = CwdFd.Open(pid, "cwd");
    if (error)
        return error;

    return OK;
}

TError TTaskEnv::ChildExec() {

    /* set environment for wordexp */
    TError error = Env.Apply();

    auto envp = Env.Envp();

    if (CT->IsMeta()) {
        const char *args[] = {
            "portoinit",
            "--container",
            CT->Name.c_str(),
            NULL,
        };
        SetDieOnParentExit(0);
        TFile::CloseAll({PortoInit.Fd, Sock.GetFd(), LogFile.Fd});
        fexecve(PortoInit.Fd, (char *const *)args, envp);
        return TError::System("cannot exec portoinit");
    }

    std::vector<const char *> argv;
    if (CT->HasProp(EProperty::COMMAND_ARGV)) {
        argv.resize(CT->CommandArgv.size() + 1);
        for (unsigned i = 0; i < CT->CommandArgv.size(); i++)
            argv[i] = CT->CommandArgv[i].c_str();
        argv.back() = nullptr;
    } else {
        wordexp_t result;

        int ret = wordexp(CT->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
        switch (ret) {
        case WRDE_BADCHAR:
            return TError(EError::InvalidCommand, "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {{, }}");
        case WRDE_BADVAL:
            return TError(EError::InvalidCommand, "wordexp(): undefined shell variable was referenced");
        case WRDE_CMDSUB:
            return TError(EError::InvalidCommand, "wordexp(): command substitution is not supported");
        case WRDE_SYNTAX:
            return TError(EError::InvalidCommand, "wordexp(): syntax error");
        default:
        case WRDE_NOSPACE:
            return TError(EError::InvalidCommand, "wordexp(): error {}", ret);
        case 0:
            break;
        }

        argv.resize(result.we_wordc + 1);
        for (unsigned i = 0; i < result.we_wordc; i++)
            argv[i] = result.we_wordv[i];
        argv.back() = nullptr;
    }

    if (Verbose) {
        L("command={}", CT->Command);
        for (unsigned i = 0; argv[i]; i++)
            L("argv[{}]={}", i, argv[i]);
        for (unsigned i = 0; envp[i]; i++)
            L("environ[{}]={}", i, envp[i]);
    }

    SetDieOnParentExit(0);
    TFile::CloseAll({0, 1, 2, Sock.GetFd(), LogFile.Fd});

    /* https://bugs.launchpad.net/upstart/+bug/1582199 */
    if (CT->Command == "/sbin/init" && CT->OsMode &&
            !(CT->Controllers & CGROUP_SYSTEMD)) {
        L_VERBOSE("Reserve fd 9 for upstart JOB_PROCESS_SCRIPT_FD");
        dup2(open("/dev/null", O_RDWR | O_CLOEXEC), 9);
    }

    L("Exec '{}'", argv[0]);
    execvpe(argv[0], (char *const *)argv.data(), envp);

    if (errno == EAGAIN)
        return TError(EError::ResourceNotAvailable, errno, "cannot exec {} not enough ulimit nproc", argv[0]);

    return TError(EError::InvalidCommand, errno, "cannot exec {}", argv[0]);
}

TError TTaskEnv::WriteResolvConf() {
    if (CT->HasProp(EProperty::RESOLV_CONF) ? !CT->ResolvConf.size() : CT->Root == "/")
        return OK;
    L_ACT("Write resolv.conf for CT{}:{}", CT->Id, CT->Name);
    return TPath("/etc/resolv.conf").WritePrivate(
            CT->ResolvConf.size() ? CT->ResolvConf : RootContainer->ResolvConf);
}

TError TTaskEnv::SetHostname() {
    TError error;

    if (CT->Hostname.size()) {
        error = TPath("/etc/hostname").WritePrivate(CT->Hostname + "\n");
        if (!error)
            error = SetHostName(CT->Hostname);
    }

    return error;
}

TError TTaskEnv::ApplySysctl() {
    TError error;

    if (CT->Isolate) {
        for (const auto &it: config().container().ipc_sysctl()) {
            error = SetSysctlAt(Mnt.ProcSysFd, it.key(), it.val());
            if (error)
                return error;
        }
    }

    for (const auto &it: CT->Sysctl) {
        auto &key = it.first;

        if (TNetwork::NetworkSysctl(key)) {
            if (!CT->NetIsolate)
                return TError(EError::Permission, "Sysctl " + key + " requires net isolation");
            continue; /* Set by TNetEnv */
        } else if (std::find(IpcSysctls.begin(), IpcSysctls.end(), key) != IpcSysctls.end()) {
            if (!CT->Isolate)
                return TError(EError::Permission, "Sysctl " + key + " requires ipc isolation");
        } else
            return TError(EError::Permission, "Sysctl " + key + " is not allowed");

        error = SetSysctlAt(Mnt.ProcSysFd, key, it.second);
        if (error)
            return error;
    }

    return OK;
}

TError TTaskEnv::ConfigureChild() {
    L("ConfigureChild");
    TError error;

    error = CT->GetUlimit().Apply();
    if (error)
        return error;

    if (setsid() < 0)
        return TError::System("setsid()");

    umask(0);

    TDevices devices = CT->Devices;
    for (auto p = CT->Parent; p; p = p->Parent)
        devices.Merge(p->Devices);

    if (NewMountNs) {

        error = Mnt.Setup(CT->CapBound.Permitted & BIT(CAP_SYS_ADMIN),
                          EnableDockerMode && CT->OwnerCred.IsRootUser(), CT->DockerMode);
        if (error)
            return error;

        for (auto &device: devices.Devices) {
            for (auto &device_sysfs: config().container().device_sysfs()) {
                if (device.Path.ToString() == device_sysfs.device()) {
                    for (auto &sysfs: device_sysfs.sysfs()) {
                        TPath path(sysfs);
                        error = path.BindRemount(path, MS_ALLOW_WRITE);
                        if (error)
                            return error;
                    }
                }
            }
        }
    }

    if (!Mnt.Root.IsRoot()) {
        error = devices.Makedev();
        if (error)
            return error;
    }

    error = ApplySysctl();
    if (error)
        return error;

    error = WriteResolvConf();
    if (error)
        return error;

    if (CT->EtcHosts.size()) {
        error = TPath("/etc/hosts").WritePrivate(CT->EtcHosts);
        if (error)
            return error;
    }

    error = SetHostname();
    if (error)
        return error;

    error = Mnt.Cwd.Chdir();
    if (error)
        return error;

    if (QuadroFork) {
        pid_t pid = Fork(config().container().ptrace_on_start());
        if (pid < 0)
            return TError::System("fork()");

        if (pid)
            ExecPortoinit(pid);

        if (setsid() < 0)
            return TError::System("setsid()");
    }

    /* Report VPid */
    if (TripleFork) {
        MasterSock2.Close();
        error = Sock2.SendPid(GetPid());
        if (error)
            return error;
        /* Wait VPid Ack */
        error = Sock2.RecvZero();
        if (error)
            return error;
        /* Parent forwards VPid */
        ReportStage++;
        Sock2.Close();
    } else
        ReportPid(GetPid());

    error = TPath("/proc/self/loginuid").WriteAll(std::to_string(LoginUid));
    if (error && error.Errno != ENOENT)
        L_WRN("Cannot set loginuid: {}", error);

    error = Cred.Apply();
    if (error)
        return error;

    if (CT->CapAmbient.Permitted)
        L("Ambient capabilities: {}", CT->CapAmbient);

    error = CT->CapAmbient.ApplyAmbient();
    if (error)
        return error;

    L("Capabilities: {}", CT->CapBound);

    error = CT->CapBound.ApplyLimit();
    if (error)
        return error;

    if (!Cred.IsRootUser()) {
        error = CT->CapAmbient.ApplyEffective();
        if (error)
            return error;
    }

    L("open default streams in child");
    error = CT->Stdin.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stdout.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stderr.OpenInside(*CT);
    if (error)
        return error;

    umask(CT->Umask);

    if (CT->DockerMode || CT->FuseMode) {
        int unshareFlags = CLONE_NEWUSER | CLONE_NEWNS;
        if (CT->DockerMode)
            unshareFlags |= CLONE_NEWNET;

        if (unshare(unshareFlags))
            return TError::System("unshare(CLONE_NEWUSER | CLONE_NEWNS{})", CT->DockerMode ? " | CLONE_NEWNET" : "");

        error = Sock.SendZero();
        if (error)
            Abort(error);

        error = Sock.RecvZero();
        if (error)
            Abort(error);
    }

    return OK;
}

TError TTaskEnv::WaitAutoconf() {
    if (Autoconf.empty())
        return OK;

    SetProcessName("portod-autoconf");

    auto sock = std::make_shared<TNl>();
    TError error = sock->Connect();
    if (error)
        return error;

    for (auto &name: Autoconf) {
        TNlLink link(sock, name);

        error = link.Load();
        if (error)
            return error;

        error = link.WaitAddress(config().network().autoconf_timeout_s());
        if (error)
            return error;
    }

    return OK;
}

void TTaskEnv::StartChild() {
    L("StartChild");
    TError error;

    if (TripleFork) {
        /* Die together with parent who report WPid */
        SetDieOnParentExit(SIGKILL);
    } else {
        /* Report WPid */
        ReportPid(GetPid());
    }

    /* Wait WPid Ack */
    error = Sock.RecvZero();
    if (error)
        Abort(error);

    /* Apply configuration */
    error = ConfigureChild();
    if (error)
        Abort(error);

    /* Wait for Wakeup */
    error = Sock.RecvZero();
    if (error)
        Abort(error);

    /* Reset signals before exec, signal block already lifted */
    ResetIgnoredSignals();

    error = WaitAutoconf();
    if (error)
        Abort(error);

    error = ChildExec();
    Abort(error);
}

void TTaskEnv::TracerLoop(pid_t traceePid) {
    TError error;
    pid_t pid;

    unsigned int remainingExecs = 1;
    if (TripleFork)
        ++remainingExecs;
    if (QuadroFork)
        ++remainingExecs;

    int status;
    if (waitpid(traceePid, &status, 0) != traceePid) {
        error = TError::System("waitpid()");
        goto tracer_error;
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        error = TError::System("Child doesn't stopped");
        goto tracer_error;
    }
    if (ptrace(PTRACE_SETOPTIONS, traceePid, 0, PTRACE_O_TRACEEXEC)) {
        error = TError::System("ptrace(PTRACE_SETOPTIONS)");
        goto tracer_error;
    }
    if (ptrace(PTRACE_CONT, traceePid, 0, 0)) {
        error = TError::System("ptrace(PTRACE_CONT)");
        goto tracer_error;
    }

    for (pid = wait(&status); pid > 0; pid = wait(&status)) {
        if (status >> 8 == (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
            --remainingExecs;
            if (ptrace(PTRACE_DETACH, pid, 0, 0))
                error = TError::System("ptrace(PTRACE_DETACH)");
        } else if (WIFSTOPPED(status)) {
            if (ptrace(PTRACE_CONT, pid, 0, 0))
                error = TError::System("ptrace(PTRACE_CONT)");
        } else if (WIFSIGNALED(status)) {
            error = TError::System("Child terminated by signal {}", WTERMSIG(status));
        }

        if (error)
            goto tracer_error;

        if (!remainingExecs)
            break;
    }
    if (remainingExecs) {
        error = TError::System("wait()");
        goto tracer_error;
    }

    _exit(EXIT_SUCCESS);

tracer_error:
    L("Tracer failed: {}", error);
    (void)kill(traceePid, SIGKILL);
    _exit(EXIT_FAILURE);
}

TError TTaskEnv::Start() {
    /* Use third fork between entering into parent pid-namespace and
    cloning isolated child pid-namespace: porto keeps waiter task inside
    which waits sub-container main task and dies in the same way. */
    L("Start with TripleFork={} QuadroFork={}", TripleFork, QuadroFork);

    TError error, error2;

    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;
    CT->SeizeTask.Pid = 0;

    error = TUnixSocket::SocketPair(MasterSock, Sock);
    if (error)
        return error;

    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    TTask task;

    error = task.Fork();
    if (error) {
        Sock.Close();
        MasterSock.Close();
        L("Can't spawn child: {}", error);
        return error;
    }

    if (!task.Pid) {
        if (config().container().ptrace_on_start()) {
            pid_t traceePid = fork();
            if (traceePid < 0)
                Abort(TError::System("fork()"));

            if (traceePid) {
                Sock.Close();
                MasterSock.Close();

                SetDieOnParentExit(SIGKILL);

                SetProcessName("portod-TRACER");


                TracerLoop(traceePid);
            }

            if (ptrace(PTRACE_TRACEME, 0, 0, 0))
                Abort(TError::System("ptrace(PTRACE_TRACEME)"));
            raise(SIGSTOP);
        }

        /* FIXME: this changes stable behaviour with starting child on reload
        MasterSock.Close(); */

        TError error;

        /* Switch from signafd back to normal signal delivery */
        ResetBlockedSignals();

        if (!config().container().ptrace_on_start())
            SetDieOnParentExit(SIGKILL);

        SetProcessName("portod-CT" + std::to_string(CT->Id));

        /* FIXME try to replace clone() with  unshare() */
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
        char stack[8192*4];
#else
        char stack[8192];
#endif

        (void)setsid();

        L("Attach to cgroups");
        // move to target cgroups
        for (auto &cg : Cgroups) {
            error = cg.Attach(GetPid());
            if (error)
                Abort(error);
        }

        error = TPath("/proc/self/oom_score_adj").WriteAll(std::to_string(CT->OomScoreAdj));
        if (error && CT->OomScoreAdj)
            Abort(error);

        L("setpriority");
        if (setpriority(PRIO_PROCESS, 0, CT->SchedNice))
            Abort(TError::System("setpriority"));

        struct sched_param param;
        param.sched_priority = CT->SchedPrio;
        if (sched_setscheduler(0, CT->SchedPolicy, &param))
            Abort(TError::System("sched_setparm"));

        if (SetIoPrio(0, CT->IoPrio))
            Abort(TError::System("ioprio"));

        L("open default streams");
        /* Default streams and redirections are outside */
        error = CT->Stdin.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        error = CT->Stdout.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        error = CT->Stderr.OpenOutside(*CT, *Client);
        if (error)
            Abort(error);

        L("Enter namespaces");

        error = IpcFd.SetNs(CLONE_NEWIPC);
        if (error)
            Abort(error);

        error = UtsFd.SetNs(CLONE_NEWUTS);
        if (error)
            Abort(error);

        error = NetFd.SetNs(CLONE_NEWNET);
        if (error)
            Abort(error);

        error = PidFd.SetNs(CLONE_NEWPID);
        if (error)
            Abort(error);

        error = MntFd.SetNs(CLONE_NEWNS);
        if (error)
            Abort(error);

        if (EnableCgroupNs) {
            error = CgFd.SetNs(CLONE_NEWCGROUP);
            if (error)
                Abort(error);
        }

        error = RootFd.Chroot();
        if (error)
            Abort(error);

        error = CwdFd.Chdir();
        if (error)
            Abort(error);

        if (TripleFork) {
            /*
             * Enter into pid-namespace. fork() hangs in libc if child pid
             * collide with parent pid outside. vfork() has no such problem.
             */
            L("vfork");
            pid_t forkPid;
            if (!config().container().ptrace_on_start())
                forkPid = vfork();
            else
                /*
                 * We can't use syscall() function from glibc
                 * because child corrupts return address on the top of the shared stack.
                 * We use inline assemlby here for clone(CLONE_VM | CLONE_VFORK | CLONE_PTRACE) syscall.
                 */
                forkPid = PtracedVfork();

            if (forkPid < 0)
                Abort(TError::System("fork()"));

            if (forkPid)
                _exit(EXIT_SUCCESS);

            error = TUnixSocket::SocketPair(MasterSock2, Sock2);
            if (error)
                Abort(error);

            /* Report WPid */
            ReportPid(GetTid());
        }

        int cloneFlags = SIGCHLD;
        if (CT->Isolate)
            cloneFlags |= CLONE_NEWPID | CLONE_NEWIPC;

        if (EnableCgroupNs && CT->OsMode)
            cloneFlags |= CLONE_NEWCGROUP;

        if (NewMountNs)
            cloneFlags |= CLONE_NEWNS;

        /* Create UTS namspace if hostname is changed or isolate=true */
        if (CT->Isolate || CT->Hostname != "")
            cloneFlags |= CLONE_NEWUTS;

        if (config().container().ptrace_on_start())
            cloneFlags |= CLONE_PTRACE;

        L("clone");
        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);

        if (clonePid < 0) {
            TError error(errno == ENOMEM ?
                         EError::ResourceNotAvailable :
                         EError::Unknown, errno, "clone()");
            Abort(error);
        }

        if (!TripleFork)
            _exit(EXIT_SUCCESS);

        /* close other side before reading */
        Sock2.Close();

        pid_t appPid, appVPid;
        error = MasterSock2.RecvPid(appPid, appVPid);
        if (error)
            Abort(error);

        /* Forward VPid */
        ReportPid(appPid);

        /* Ack VPid */
        error = MasterSock2.SendZero();
        if (error)
            Abort(error);

        MasterSock2.Close();

        ExecPortoinit(clonePid);
    }

    Sock.Close();

    error = MasterSock.SetRecvTimeout(config().container().start_timeout_ms());
    if (error)
        goto kill_all;

    error = MasterSock.RecvPid(CT->WaitTask.Pid, CT->TaskVPid);
    if (error) {
        if (errno == EWOULDBLOCK) {
            Statistics->StartTimeouts++;
            PrintStack(task.Pid);
        }
        goto kill_all;
    }

    /* Ack WPid */
    error = MasterSock.SendZero();
    if (error)
        goto kill_all;

    error = MasterSock.RecvPid(CT->Task.Pid, CT->TaskVPid);
    if (error) {
        if (errno == EWOULDBLOCK) {
            Statistics->StartTimeouts++;
            PrintStack(CT->WaitTask.Pid);
        }
        goto kill_all;
    }

    if (!config().container().ptrace_on_start())
        error2 = task.Wait();

    /* Task was alive, even if it already died we'll get zombie */

    if (CT->DockerMode || CT->FuseMode) {
        // wait joining user namespace
        error = MasterSock.RecvZero();
        if (error)
            Abort(error);

        error = CT->TaskCred.SetupMapping(CT->Task.Pid, CT->FuseMode);
        if (error)
            Abort(error);

        if (CT->DockerMode) {
            error = TNetwork::StartNetwork(*CT, *this);
            if (error)
                Abort(error);
        }

        error = MasterSock.SendZero();
        if (error)
            Abort(error);
    }

    error = MasterSock.SendZero();
    if (error)
        L("Task wakeup error: {}", error);

    /* Prefer reported error if any */
    error = MasterSock.RecvError();
    if (error) {
        if (errno == EWOULDBLOCK) {
            Statistics->StartTimeouts++;
            PrintStack(CT->Task.Pid);
        }
        goto kill_all;
    }

    if (config().container().ptrace_on_start()) {
        error = task.Wait();
        if (error)
            goto kill_all;
    } else if (!error && error2) {
        error = error2;
        goto kill_all;
    }

    return OK;

kill_all:
    L("Task start failed: {}", error);
    if (task.Pid) {
        task.Kill(SIGKILL);
        task.Wait();
    }
    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;
    CT->SeizeTask.Pid = 0;
    return error;
}

void TTaskEnv::ExecPortoinit(pid_t pid) {
    auto pid_ = std::to_string(pid);
    const char * argv[] = {
        "portoinit",
        "--container",
        CT->Name.c_str(),
        "--wait",
        pid_.c_str(),
        NULL,
    };
    auto envp = Env.Envp();

    TError error = PortoInitCapabilities.ApplyLimit();
    if (!error) {
        TFile::CloseAll({PortoInit.Fd, LogFile.Fd});
        L("Exec portoinit");
        fexecve(PortoInit.Fd, (char *const *)argv, envp);
        error = TError::System("fexecve");
    }

    L("Cannot exec portoinit: {}", error);
    kill(pid, SIGKILL);
    _exit(EXIT_FAILURE);
}
