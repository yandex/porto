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
}

using std::stringstream;
using std::string;
using std::vector;
using std::map;

// TTaskEnv
void TTaskEnv::ParseEnv() {
    stringstream ss;
    for (auto i = Environ.begin(); i != Environ.end(); i++) {
        if (*i == '\\' && (i + 1) != Environ.end() && *(i + 1) == ';') {
            ss << ';';
            i++;
        } else if (*i == ';') {
            if (ss.str().length())
                EnvVec.push_back(ss.str());
            ss.str("");
        } else {
            ss << *i;
        }
    }

    if (ss.str().length())
        EnvVec.push_back(ss.str());
}

TError TTaskEnv::Prepare() {
    if (Command.empty())
        return TError::Success();

    EnvVec.push_back("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    ParseEnv();
    EnvVec.push_back("HOME=" + Cwd);
    EnvVec.push_back("USER=" + User);

    TUser u(User);
    TError error = u.Load();
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
    TError e = out.Remove();
    TLogger::LogError(e, "Can't remove task stdout " + Env->StdoutPath);

    TFile err(Env->StderrPath);
    e = err.Remove();
    TLogger::LogError(e, "Can't remove task stderr " + Env->StderrPath);
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

void TTask::OpenStdFile(const std::string &path, int expected) {
    int ret = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0700);
    if (ret < 0)
        Abort(errno, "open(" + path + ") -> " + std::to_string(expected));

    if (ret != expected)
        Abort(EINVAL, "open(" + path + ") -> " +
              std::to_string(expected) + ": unexpected fd");

    ret = fchown(ret, Env->Uid, Env->Gid);
    if (ret < 0)
        Abort(errno, "fchown(" + path + ") -> " +
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

    int ret = open(Env->StdinPath.c_str(), O_CREAT | O_RDONLY, 0700);
    if (ret < 0)
        Abort(errno, "open(0)");

    if (ret != 0)
        Abort(EINVAL, "open(0): unexpected fd");

    OpenStdFile(Env->StdoutPath, 1);
    OpenStdFile(Env->StderrPath, 2);
}

void TTask::ChildDropPriveleges() {
    if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) < 0)
        Abort(errno, "prctl(PR_SET_KEEPCAPS)");

    if (setgid(Env->Gid) < 0)
        Abort(errno, "setgid()");

    if (initgroups(Env->User.c_str(), Env->Gid) < 0)
        Abort(errno, "initgroups()");

    if (setuid(Env->Uid) < 0)
        Abort(errno, "setuid()");

    umask(0);
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

void TTask::BindDns() {
    vector<string> files = { "/etc/hosts", "/etc/resolv.conf" };

    bool filesNr = false;

    for (auto &path : files) {
        TFile file(Env->Root + path);
        if (file.Exists())
            filesNr++;
    }

    if (!filesNr)
        return;

    for (auto &path : files) {
        TFile file(Env->Root + path);

        TError error = file.Touch();
        if (error)
            Abort(error, "touch(" + file.GetPath() + ")");

        if (file.Type() == TFile::Link) {
            // TODO: ?can't mount over link
        }

        TMount mnt(path, file.GetPath(), "none", {});
        if (mnt.Bind())
            Abort(errno, "bind(" + file.GetPath() + " -> " + path + ")");
    }
}

void TTask::ChildIsolateFs() {
    if (Env->Root == "/")
        return;

    vector<string> bindDir = { "/sys", "/dev" };

    for (auto &path : bindDir) {
        TFile file(path);
        TFolder dir(Env->Root + path);
        if (!dir.Exists()) {
            TError error = dir.Create();
            if (error)
                Abort(error);
        }

        TMount mnt(path, dir.GetPath(), "none", {});
        if (mnt.Bind())
            Abort(errno, "bind(" + dir.GetPath() + " -> " + path + ")");
    }

    TFolder procDir(Env->Root + "/proc");
    if (!procDir.Exists()) {
        TError error = procDir.Create();
        if (error)
            Abort(error);
    }

    TMount newProc("proc", procDir.GetPath(), "proc", {});
    TError error = newProc.Mount();
    if (error)
        Abort(error);

    BindDns();

    if (chdir(Env->Root.c_str()) < 0)
        Abort(errno, "chdir()");

    if (chroot(Env->Root.c_str()) < 0)
        Abort(errno, "chroot()");

    if (chdir("/") < 0)
        Abort(errno, "chdir()");
}

void TTask::ApplyLimits() {
    for (auto pair : Env->Rlimit) {
        int ret = setrlimit(pair.first, &pair.second);
        if (ret < 0)
            Abort(errno, "setrlimit(" + std::to_string(pair.first) + ", " + std::to_string(pair.second.rlim_cur) + ":" + std::to_string(pair.second.rlim_max) + ")");
    }
}

int TTask::ChildCallback() {
    close(Rfd);
    ResetAllSignalHandlers();
    ApplyLimits();

    if (setsid() < 0)
        Abort(errno, "setsid()");

    if (Env->Isolate) {
        // remount proc so PID namespace works
        TMount proc("proc", "/proc", "proc", {});
        if (proc.Mount())
            Abort(errno, "remount procfs");
    }

    // move to target cgroups
    for (auto cg : LeafCgroups) {
        auto error = cg->Attach(getpid());
        if (error)
            Abort(error, "cgroup attach");
    }

    ChildReopenStdio();
    ChildIsolateFs();

    if (chdir(Env->Cwd.c_str()) < 0)
        Abort(errno, "chdir()");

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
    return Cwd->Chown(Env->User, Env->Group);
}

TError TTask::Start() {
    int ret;
    int pfd[2];

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
            cloneFlags |= CLONE_NEWPID | CLONE_NEWNS;

        pid_t clonePid = clone(ChildFn, stack + sizeof(stack), cloneFlags, this);
        if (clonePid < 0) {
            TError error(EError::Unknown, errno, "clone()");
            TLogger::LogError(error, "Can't spawn child");
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

    TFile stdinLink("/proc/" + std::to_string(pid_) + "/fd/0");
    TError error = stdinLink.ReadLink(Env->StdinPath);
    if (error)
        Env->StdinPath = "";
    TLogger::LogError(error, "Restore stdin");

    TFile stdoutLink("/proc/" + std::to_string(pid_) + "/fd/1");
    error = stdoutLink.ReadLink(Env->StdoutPath);
    if (error)
        Env->StdoutPath = Env->Cwd + "/stdout";
    TLogger::LogError(error, "Restore stdout");

    TFile stderrLink("/proc/" + std::to_string(pid_) + "/fd/2");
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

TError TTask::RotateFile(const std::string path) const {
    struct stat st;

    if (!path.length())
        return TError::Success();

    if (stat(path.c_str(), &st) < 0)
        return TError(EError::Unknown, errno, "stat(" + path + ")");

    if (!S_ISREG(st.st_mode))
        return TError::Success();

    if (st.st_size > config().container().max_log_size())
        if (truncate(path.c_str(), 0) < 0)
            return TError(EError::Unknown, errno, "truncate(" + path + ")");

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
