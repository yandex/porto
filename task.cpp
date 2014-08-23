#include <climits>
#include <sstream>
#include <iterator>
#include <csignal>

#include "porto.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "log.hpp"
#include "util/string.hpp"

extern "C" {
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <wordexp.h>
}

using namespace std;

// TTaskEnv
TError TTaskEnv::Prepare() {
    if (command.empty())
        return TError::Success();

    string workdir;
    if (cwd.length())
        workdir = cwd;
    else
        workdir = "/home/" + user;

    env.push_back("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:" + workdir);

    if (SplitString(envir, ';', env)) {
        TError error(EError::InvalidValue, errno, "split(" + envir + ")");
        return error;
    }

    env.push_back("HOME=" + workdir);
    env.push_back("USER=" + user);

    struct passwd *p = getpwnam(user.c_str());
    if (!p) {
        TError error(EError::InvalidValue, EINVAL, "getpwnam(" + user + ")");
        return error;
    } else {
        uid = p->pw_uid;
    }

    struct group *g = getgrnam(group.c_str());
    if (!g) {
        TError error(EError::InvalidValue, EINVAL, "getgrnam(" + group + ")");
        return error;
    } else {
        gid = g->gr_gid;
    }

    return TError::Success();
}

const char** TTaskEnv::GetEnvp() {
    auto envp = new const char* [env.size() + 1];
    for (size_t i = 0; i < env.size(); i++)
        envp[i] = env[i].c_str();
    envp[env.size()] = NULL;

    return envp;
}

// TTask
int TTask::CloseAllFds(int except) {
    for (int i = 0; i < getdtablesize(); i++)
        if (i != except)
            close(i);

    return except;
}

void TTask::ReportResultAndExit(int fd, int result)
{
    if (write(fd, &result, sizeof(result))) {}
    exit(EXIT_FAILURE);
}

void TTask::Syslog(const string &s)
{
    openlog("portod", LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_ERR, "%s", s.c_str());
    closelog();
}

TTask::~TTask() {
    if (stdoutFile.length()) {
        TFile f(stdoutFile);
        TError e = f.Remove();
        TLogger::LogError(e, "Can't remove task stdout " + stdoutFile);
    }
    if (stderrFile.length()) {
        TFile f(stderrFile);
        TError e = f.Remove();
        TLogger::LogError(e, "Can't remove task stderr " + stdoutFile);
    }
}

static string GetTmpFile() {
    char p[] = "/tmp/XXXXXX";
    int fd = mkstemp(p);
    if (fd < 0)
        return "";

    string path(p);
    close(fd);
    return path;
}

static int child_fn(void *arg) {
    TTask *task = static_cast<TTask*>(arg);
    return task->ChildCallback();
}

int TTask::ChildCallback() {
    close(rfd);

    /*
     * ReportResultAndExit(fd, -errno) means we failed while preparing
     * to execve, which should never happen (but it will :-)
     *
     * ReportResultAndExit(fd, +errno) means execve failed
     */

    if (prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) < 0) {
        Syslog(string("prctl(PR_SET_KEEPCAPS): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    if (setsid() < 0) {
        Syslog(string("setsid(): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    // remount proc so PID namespace works
    TMount proc("proc", "/proc", "proc", {});
    if (proc.Remount()) {
        Syslog(string("remount procfs: ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    // move to target cgroups
    for (auto cg : leaf_cgroups) {
        Syslog(string("attach ") + to_string(getpid()));

        auto error = cg->Attach(getpid());
        if (error) {
            Syslog(string("cgroup attach: ") + error.GetMsg());
            ReportResultAndExit(wfd, -error.GetError());
        }
    }

    wfd = CloseAllFds(wfd);
    if (wfd < 0) {
        Syslog(string("close fds: ") + strerror(errno));
        /* there is no way of telling parent that we failed (because we
         * screwed up fds), so exit with some eye catching error code */
        exit(0xAA);
    }

    int ret = open("/dev/null", O_RDONLY);
    if (ret < 0) {
        Syslog(string("open(0): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    ret = open(stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0700);
    if (ret < 0) {
        Syslog(string("open(1): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    ret = fchown(ret, env.uid, env.gid);
    if (ret < 0) {
        Syslog(string("fchown(1): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    ret = open(stderrFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0700);
    if (ret < 0) {
        Syslog(string("open(2): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    ret = fchown(ret, env.uid, env.gid);
    if (ret < 0) {
        Syslog(string("fchown(2): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    TMount new_root(env.root, env.root + "/", "none", {});
    TMount new_proc("proc", env.root + "/proc", "proc", {});
    TMount new_sys("/sys", env.root + "/sys", "none", {});
    TMount new_dev("/dev", env.root + "/dev", "none", {});
    TMount new_var("/var", env.root + "/var", "none", {});
    TMount new_run("/run", env.root + "/run", "none", {});
    TMount new_tmp("/tmp", env.root + "/tmp", "none", {});

    if (env.root.length()) {
        if (new_root.Bind()) {
            Syslog(string("remount /: ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (new_tmp.Bind()) {
            Syslog(string("remount /tmp: ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (new_sys.Bind()) {
            Syslog(string("remount /sys: ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (new_run.Bind()) {
            Syslog(string("remount /run: ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (new_dev.Bind()) {
            Syslog(string("remount /dev: ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (new_var.Bind()) {
            Syslog(string("remount /var: ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (new_proc.Mount()) {
            Syslog(string("remount /proc: ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (chdir(env.root.c_str()) < 0) {
            Syslog(string("chdir(): ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (chroot(env.root.c_str()) < 0) {
            Syslog(string("chroot(): ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

        if (chdir("/") < 0) {
            Syslog(string("chdir(): ") + strerror(errno));
            ReportResultAndExit(wfd, -errno);
        }

    }

    if (env.cwd.length() && chdir(env.cwd.c_str()) < 0) {
        Syslog(string("chdir(): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    // drop privileges
    if (setgid(env.gid) < 0) {
        Syslog(string("setgid(): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    if (initgroups(env.user.c_str(), env.gid) < 0) {
        Syslog(string("initgroups(): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    if (setuid(env.uid) < 0) {
        Syslog(string("setuid(): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    umask(0);
    clearenv();

	wordexp_t result;

	ret = wordexp(env.command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
    switch (ret) {
    case WRDE_BADCHAR:
        Syslog(string("wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {, }"));
        ReportResultAndExit(wfd, -EINVAL);
    case WRDE_BADVAL:
        Syslog(string("wordexp(): undefined shell variable was referenced"));
        ReportResultAndExit(wfd, -EINVAL);
    case WRDE_CMDSUB:
        Syslog(string("wordexp(): command substitution is not supported"));
        ReportResultAndExit(wfd, -EINVAL);
    case WRDE_SYNTAX:
        Syslog(string("wordexp(): syntax error"));
        ReportResultAndExit(wfd, -EINVAL);
    default:
    case WRDE_NOSPACE:
        Syslog(string("wordexp(): error ") + strerror(ret));
        ReportResultAndExit(wfd, -EINVAL);
    case 0:
        break;
    }

#ifdef __DEBUG__
    for (int i = 0; i < result.we_wordc; i++)
        Syslog(result.we_wordv[i]);
#endif

    auto envp = env.GetEnvp();
    execvpe(result.we_wordv[0], (char *const *)result.we_wordv, (char *const *)envp);

    Syslog(string("execvpe(): ") + strerror(errno));
    ReportResultAndExit(wfd, errno);

    return 0;
}

TError TTask::Start() {
    int ret;
    int pfd[2];

    exitStatus.error = 0;
    exitStatus.status = 0;

    if (env.cwd.length()) {
        stdoutFile = env.cwd + "/stdout";
        stderrFile = env.cwd + "/stderr";
    } else {
        stdoutFile = GetTmpFile();
        stderrFile = GetTmpFile();
    }

    ret = pipe2(pfd, O_CLOEXEC);
    if (ret) {
        TError error(EError::Unknown, errno, "pipe2(pdf)");
        TLogger::LogError(error, "Can't create communication pipe for child");
        return error;
    }

    rfd = pfd[0];
    wfd = pfd[1];

    char stack[8192];
    pid_t pid = clone(child_fn, stack + sizeof(stack),
                      SIGCHLD | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS, this);
    if (pid < 0) {
        TError error(EError::Unknown, errno, "fork()");
        TLogger::LogError(error, "Can't spawn child");
        return error;
    }

    close(wfd);
    int n = read(rfd, &ret, sizeof(ret));
    close(rfd);
    if (n < 0) {
        TError error(EError::Unknown, errno, "read(rfd)");
        TLogger::LogError(error, "Can't read result from the child");
        return error;
    } else if (n == 0) {
        state = Started;
        this->pid = pid;
        return TError::Success();
    } else {
        this->pid = pid;
        TError error = Reap(true);
        if (error)
            TLogger::LogError(error, "Couldn't reap child process");
        this->pid = 0;

        exitStatus.error = ret;
        exitStatus.status = -1;

        if (ret < 0)
            error = TError(EError::Unknown, string("child prepare: ") + strerror(-ret));
        else
            error = TError(EError::Unknown, string("child exec: ") + strerror(ret));
        TLogger::LogError(error, "Child process couldn't exec");
        return error;
    }
}

int TTask::GetPid() {
    return pid;
}

bool TTask::IsRunning() {
    if (state == Started)
        (void)Reap(false);

    return state == Started;
}

TExitStatus TTask::GetExitStatus() {
    if (state == Started)
        (void)Reap(false);

    return exitStatus;
}

bool TTask::CanReap() {
    // May give false-positive when task is running, but we are not its
    // parent
    return kill(pid, 0) == 0;
}

TError TTask::Reap(bool wait) {
    int status;
    pid_t ret;

    ret = waitpid(pid, &status, wait ? 0 : WNOHANG);
    TLogger::Log("reap(" + to_string(pid) + ") = " + to_string(ret));
    if (ret == pid) {
        DeliverExitStatus(status);
    } else if (ret < 0) {
        if (kill(pid, 0) == 0) {
            // process is still running but we can't use wait() on it
            // (probably from the previous session) -> state should
            // be changed via DeliverExitStatus

            TLogger::Log(to_string(pid) + " is still running, will wait for status delivery" );
            return TError::Success();
        }

        exitStatus.error = -1;
        state = Stopped;
        return TError(EError::Unknown, errno, "waitpid()");
    }

    return TError::Success();
}

void TTask::DeliverExitStatus(int status) {
    exitStatus.error = 0;
    exitStatus.status = status;
    state = Stopped;
}

void TTask::Kill(int signal) {
    if (!pid)
        throw "Tried to kill invalid process!";

    int ret = kill(pid, signal);
    if (ret != 0) {
        TError error(EError::Unknown, errno, "kill(" + to_string(pid) + ")");
        TLogger::LogError(error, "Can't kill child process");
    }
}

std::string TTask::GetStdout() {
    string s;
    TFile f(stdoutFile);
    TError e(f.LastStrings(STDOUT_READ_BYTES, s));
    TLogger::LogError(e, "Can't read container stdout");
    return s;
}

std::string TTask::GetStderr() {
    string s;
    TFile f(stderrFile);
    TError e(f.LastStrings(STDOUT_READ_BYTES, s));
    TLogger::LogError(e, "Can't read container stderr");
    return s;
}

TError TTask::Restore(int pid_) {
    exitStatus.error = 0;
    exitStatus.status = 0;

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

    // TODO: look for stdout/stderr in standard places in case we fail

    TFile stdoutLink("/proc/" + to_string(pid_) + "/fd/1");
    TError error = stdoutLink.ReadLink(stdoutFile);
    TLogger::LogError(error, "Restore stdout");

    TFile stderrLink("/proc/" + to_string(pid_) + "/fd/2");
    error = stderrLink.ReadLink(stderrFile);
    TLogger::LogError(error, "Restore stderr");

    pid = pid_;
    state = Started;

    error = ValidateCgroups();
    TLogger::LogError(error, "Can't validate cgroups");

    return TError::Success();
}

TError TTask::ValidateCgroups() {
    TFile f("/proc/" + to_string(pid) + "/cgroup");

    vector<string> lines;
    map<string, string> cgmap;
    TError error = f.AsLines(lines);
    if (error)
        return error;

    vector<string> tokens;
    for (auto l : lines) {
        tokens.clear();
        error = SplitString(l, ':', tokens);
        if (error)
            return error;

        const string &subsys = tokens[1];
        const string &path = tokens[2];

        bool valid = false;
        for (auto cg : leaf_cgroups) {
            if (cg->HasSubsystem(subsys)) {
                if (cg->Relpath() == path) {
                    valid = true;
                    break;
                }
            }
        }

        if (!valid)
            return TError(EError::Unknown, "Task belongs to invalid subsystem " + subsys + ":" + path);
    }

    return TError::Success();
}

TError TTask::RotateFile(const std::string path) {
    struct stat st;

    if (stat(path.c_str(), &st) < 0)
        return TError(EError::Unknown, errno, "stat(" + path + ")");

    if (st.st_size > CONTAINER_MAX_LOG_SIZE)
        if (truncate(path.c_str(), 0) < 0)
            return TError(EError::Unknown, errno, "truncate(" + path + ")");

    return TError::Success();
}

TError TTask::Rotate() {
    TError error;

    error = RotateFile(stdoutFile);
    if (error)
        return error;

    error = RotateFile(stderrFile);
    if (error)
        return error;

    return TError::Success();
}
