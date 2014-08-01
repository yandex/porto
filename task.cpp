#include <climits>

#include "task.hpp"
#include "cgroup.hpp"
#include "log.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
}

TTask::TTask(string &path, vector<string> &args) : path(path), args(args)
{
}

int TTask::CloseAllFds(int except)
{
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

const char** TTask::GetArgv()
{
    auto argv = new const char* [args.size() + 2];
    argv[0] = path.c_str();
    for (size_t i = 0; i < args.size(); i++)
        argv[i + 1] = args[i].c_str();
    argv[args.size() + 1] = NULL;

    return argv;
}

bool TTask::Start() {
    lock_guard<mutex> guard(lock);

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
            return -errno;

        // TODO: move to cgroups
        // chdir(controller->root);

        wfd = CloseAllFds(wfd);
        if (wfd < 0)
            return wfd;

        auto argv = GetArgv();
        execvp(path.c_str(), (char *const *)argv);

        if (write(wfd, &errno, sizeof(errno)) == 0) {}
        exit(EXIT_FAILURE);
    }

    close(wfd);

    int n = read(rfd, &ret, sizeof(ret));
    if (n < 0) {
        TLogger::LogAction("read child status failed", true, errno);
        exitStatus.error = errno;
        return false;
    } else if (n == 0) {
        state = Running;
        this->pid = pid;
        return true;
    } else {
        TLogger::LogAction("got status from child", true, errno);
        (void)waitpid(pid, NULL, WNOHANG);
        exitStatus.error = ret;
        return false;
    }
}

void TTask::FindCgroups()
{
}

int TTask::GetPid()
{
    lock_guard<mutex> guard(lock);

    if (state == Running)
        return pid;
    else
        return 0;
}

bool TTask::IsRunning()
{
    GetExitStatus();

    lock_guard<mutex> guard(lock);
    return state == Running;
}

TExitStatus TTask::GetExitStatus()
{
    lock_guard<mutex> guard(lock);

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

void TTask::Kill()
{
    lock_guard<mutex> guard(lock);

    int ret = kill(pid, SIGTERM);
    if (ret == ESRCH)
        return;

    // TODO: add some sleep before killing with -9 ?

    kill(pid, SIGKILL);
}
