#include <climits>
#include <sstream>
#include <iterator>

#include "task.hpp"
#include "cgroup.hpp"
#include "log.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <wordexp.h>
}

using namespace std;

// TTaskEnv
    TTaskEnv::TTaskEnv(const std::string &command, const std::string &cwd, const std::string &user, const std::string &group, const std::string &envir) : command(command) {
    // TODO: support quoting
    if (command.empty())
        return;

    {
        istringstream s(envir);
        string token;

        env.push_back("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
        env.push_back("HOME=/home/" + user);
        env.push_back("USER=" + user);

        while (getline(s, token, ';'))
            env.push_back(token);
    }

    struct passwd *p = getpwnam(user.c_str());
    if (!p) {
        uid = 65534;
        TLogger::LogAction("getpwnam " + user, true, errno);
    } else {
        uid = p->pw_uid;
    }

    struct group *g = getgrnam(group.c_str());
    if (!g) {
        TLogger::LogAction("getgrnam " + user, true, errno);
        gid = 65534;
    } else {
        gid = g->gr_gid;
    }
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
    close(0);
    except = dup3(except, 0, O_CLOEXEC);
    if (except < 0)
        return except;

    for (int i = 1; i < getdtablesize(); i++)
        close(i);

    except = dup3(except, 3, O_CLOEXEC);
    if (except < 0)
        return except;
    close(0);

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
        if (e)
            TLogger::LogError(e, "Can't remove task stdout " + stdoutFile);
    }
    if (stderrFile.length()) {
        TFile f(stderrFile);
        TError e = f.Remove();
        if (e)
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
    TMount proc("proc", "/proc", "proc", 0, {});
    if (proc.Mount()) {
        Syslog(string("remount procfs: ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

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

    if (env.cwd.length() && chdir(env.cwd.c_str()) < 0) {
        Syslog(string("chdir(): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    for (auto cg : leaf_cgroups) {
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

    // TODO: O_APPEND to support rotation?
    ret = open(stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (ret < 0) {
        Syslog(string("open(1): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    ret = fchown(ret, env.uid, env.gid);
    if (ret < 0) {
        Syslog(string("fchown(1): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    // TODO: O_APPEND to support rotation?
    ret = open(stderrFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (ret < 0) {
        Syslog(string("open(2): ") + strerror(errno));
        ReportResultAndExit(wfd, -errno);
    }

    ret = fchown(ret, env.uid, env.gid);
    if (ret < 0) {
        Syslog(string("fchown(2): ") + strerror(errno));
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
    exitStatus.signal = 0;
    exitStatus.status = 0;

    // TODO: use real container root directory
    stdoutFile = GetTmpFile();
    stderrFile = GetTmpFile();

    ret = pipe2(pfd, O_CLOEXEC);
    if (ret) {
        TLogger::LogAction("pipe2", ret == 0, errno);
        return TError(TError::Unknown, errno);
    }

    rfd = pfd[0];
    wfd = pfd[1];

    char stack[8192];
    pid_t pid = clone(child_fn, stack + sizeof(stack),
                      CLONE_NEWNS | CLONE_NEWPID, this);
    if (pid < 0) {
        TLogger::LogAction("fork", ret == 0, errno);
        return TError(TError::Unknown, errno);
    }

    close(wfd);

    int n = read(rfd, &ret, sizeof(ret));
    if (n < 0) {
        TLogger::LogAction("read child status failed", false, errno);
        return TError(TError::Unknown, errno);
    } else if (n == 0) {
        state = Running;
        this->pid = pid;
        return NoError;
    } else {
        TLogger::LogAction("got status from child", false, errno);
        (void)waitpid(pid, NULL, WNOHANG);
        exitStatus.error = ret;
        return TError(TError::Unknown);
    }
}

void TTask::FindCgroups() {
}

int TTask::GetPid() {
    if (state == Running)
        return pid;
    else
        return 0;
}

bool TTask::IsRunning() {
    GetExitStatus();

    return state == Running;
}

TExitStatus TTask::GetExitStatus() {
    if (state != Stopped) {
        int status;
        pid_t ret;
        ret = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (ret) {
            if (!exitStatus.error) {
                exitStatus.signal = WTERMSIG(status);
                exitStatus.status = WEXITSTATUS(status);
            }
            state = Stopped;
        }
    }

    return exitStatus;
}

void TTask::Reap() {
    int ret = waitpid(pid, NULL, 0);
    TLogger::LogAction("wait " + to_string(pid) + " ret=" + to_string(ret), ret != pid, errno);
}

void TTask::Kill(int signal) {
    if (!pid)
        throw "Tried to kill invalid process!";

    int ret = kill(pid, signal);
    TLogger::LogAction("kill " + to_string(pid) + " ret=" + to_string(ret), ret != 0, errno);
}

std::string TTask::GetStdout() {
    string s;
    TFile f(stdoutFile);
    TError e = f.AsString(s);
    if (e)
        TLogger::LogError(e);
    return s;
}

std::string TTask::GetStderr() {
    string s;
    TFile f(stderrFile);
    TError e = f.AsString(s);
    if (e)
        TLogger::LogError(e);
    return s;
}
