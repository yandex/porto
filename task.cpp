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
}

using namespace std;

// TTaskEnv
TTaskEnv::TTaskEnv(const std::string command, const string cwd) : cwd(cwd) {
    // TODO: support quoting
    if (command.empty())
        return;

    istringstream s(command);
    args.insert(args.end(),
                istream_iterator<string>(s),
                istream_iterator<string>());

    path = args.front();
    args.erase(args.begin());
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

    for (int i = 0; i < 3; i++)
        open("/dev/null", O_RDWR);

    return except;
}

const char** TTask::GetArgv() {
    auto argv = new const char* [env.args.size() + 2];
    argv[0] = env.path.c_str();
    for (size_t i = 0; i < env.args.size(); i++)
        argv[i + 1] = env.args[i].c_str();
    argv[env.args.size() + 1] = NULL;

    return argv;
}

void TTask::ReportResultAndExit(int fd, int result)
{
    if (write(fd, &result, sizeof(result))) {}
    exit(EXIT_FAILURE);
}

bool TTask::Start() {
    int ret;
    int pfd[2];

    exitStatus.error = 0;
    exitStatus.signal = 0;
    exitStatus.status = 0;

    ret = pipe2(pfd, O_CLOEXEC);
    if (ret) {
        TLogger::LogAction("pipe2", ret == 0, errno);
        exitStatus.error = errno;
        return false;
    }

    int rfd = pfd[0];
    int wfd = pfd[1];

    pid_t pid = fork();

    if (pid < 0) {
        TLogger::LogAction("fork", ret == 0, errno);
        exitStatus.error = errno;
        return false;
    } else if (pid == 0) {
        close(rfd);

        if (setsid() < 0)
            ReportResultAndExit(wfd, -errno);

        if (env.cwd.length() && chdir(env.cwd.c_str()) < 0)
            ReportResultAndExit(wfd, -errno);

        fork_hook();

        wfd = CloseAllFds(wfd);
        if (wfd < 0)
            exit(wfd);

        auto argv = GetArgv();
        execvp(argv[0], (char *const *)argv);

        ReportResultAndExit(wfd, errno);
    }

    close(wfd);

    int n = read(rfd, &ret, sizeof(ret));
    if (n < 0) {
        TLogger::LogAction("read child status failed", false, errno);
        exitStatus.error = errno;
        return false;
    } else if (n == 0) {
        state = Running;
        this->pid = pid;
        return true;
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
    int ret = kill(pid, signal);
    if (ret == ESRCH)
        return;
}
