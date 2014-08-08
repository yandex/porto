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
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
}

using namespace std;

// TTaskEnv
TTaskEnv::TTaskEnv(const std::string &command, const std::string &cwd, const std::string &user, const std::string &group) : cwd(cwd), user(user), group(group) {
    // TODO: support quoting
    if (command.empty())
        return;

    istringstream s(command);
    args.insert(args.end(),
                istream_iterator<string>(s),
                istream_iterator<string>());

    path = args.front();
    args.erase(args.begin());

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

const char** TTaskEnv::GetArgv() {
    auto argv = new const char* [args.size() + 2];
    argv[0] = path.c_str();
    for (size_t i = 0; i < args.size(); i++)
        argv[i + 1] = args[i].c_str();
    argv[args.size() + 1] = NULL;

    return argv;
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
        return TError(errno);
    }

    int rfd = pfd[0];
    int wfd = pfd[1];

    pid_t pid = fork();

    if (pid < 0) {
        TLogger::LogAction("fork", ret == 0, errno);
        return TError(errno);

    } else if (pid == 0) {
        close(rfd);

        /*
         * ReportResultAndExit(fd, -errno) means we failed while preparing
         * to execve, which should never happen (but it will :-)
         *
         * ReportResultAndExit(fd, +errno) means execve failed
         */

        if (setsid() < 0)
            ReportResultAndExit(wfd, -errno);

        if (env.cwd.length() && chdir(env.cwd.c_str()) < 0)
            ReportResultAndExit(wfd, -errno);

        for (auto cg : leaf_cgroups) {
            auto error = cg->Attach(getpid());
            if (error)
                ReportResultAndExit(wfd, -error.GetError());
        }

        wfd = CloseAllFds(wfd);
        if (wfd < 0)
            /* there is no way of telling parent that we failed (because we
             * screwed up fds), so exit with some eye catching error code */
            exit(0xAA);

        ret = open("/dev/null", O_RDONLY);
        if (ret < 0)
            ReportResultAndExit(wfd, -errno);

        // TODO: O_APPEND to support rotation?
        ret = open(stdoutFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0700);
        if (ret < 0)
            ReportResultAndExit(wfd, -errno);

        ret = fchown(ret, env.uid, env.gid);
        if (ret < 0)
            ReportResultAndExit(wfd, -errno);

        // TODO: O_APPEND to support rotation?
        ret = open(stderrFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0700);
        if (ret < 0)
            ReportResultAndExit(wfd, -errno);

        ret = fchown(ret, env.uid, env.gid);
        if (ret < 0)
            ReportResultAndExit(wfd, -errno);

        // drop privileges
        if (setgid(env.gid) < 0)
            ReportResultAndExit(wfd, -errno);

        if (initgroups(env.user.c_str(), env.gid) < 0)
            ReportResultAndExit(wfd, -errno);

        if (setuid(env.uid) < 0)
            ReportResultAndExit(wfd, -errno);

        umask(0);

        auto argv = env.GetArgv();
        execvp(argv[0], (char *const *)argv);

        ReportResultAndExit(wfd, errno);
    }

    close(wfd);

    int n = read(rfd, &ret, sizeof(ret));
    if (n < 0) {
        TLogger::LogAction("read child status failed", false, errno);
        return TError(errno);
    } else if (n == 0) {
        state = Running;
        this->pid = pid;
        return TError();
    } else {
        TLogger::LogAction("got status from child", false, errno);
        (void)waitpid(pid, NULL, WNOHANG);
        exitStatus.error = ret;
        return false;
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
            exitStatus.signal = WTERMSIG(status);
            exitStatus.status = WEXITSTATUS(status);
            state = Stopped;
        }
    }

    return exitStatus;
}

void TTask::Kill(int signal) {
    if (!pid)
        throw "Tried to kill invalid process!";

    int ret = kill(pid, signal);

    TLogger::LogAction("kill " + to_string(pid), ret, errno);

    if (ret == ESRCH)
        return;
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
