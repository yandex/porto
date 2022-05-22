#include <condition_variable>
#include <mutex>

#include "task.hpp"
#include "unix.hpp"
#include "log.hpp"
#include "namespace.hpp"

extern "C" {
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
}

#ifndef PR_TRANSLATE_PID
#define PR_TRANSLATE_PID    0x59410001
#endif

bool TTask::Exists() const {
    return Pid && (!kill(Pid, 0) || errno != ESRCH);
}

TError TTask::Kill(int signal) const {
    if (!Pid)
        return TError("Task is not running");
    L_ACT("kill {} {}", signal, Pid);
    if (kill(Pid, signal))
        return TError::System("kill(" + std::to_string(Pid) + ")");
    return OK;
}

TError TTask::KillPg(int signal) const {
    if (!Pid)
        return TError("Task is not running");
    L_ACT("killpg {} {}", signal, Pid);
    if (killpg(Pid, signal))
        return TError::System("killpg(" + std::to_string(Pid) + ")");
    return OK;
}

bool TTask::IsZombie() const {
    std::string path = "/proc/" + std::to_string(Pid) + "/stat";
    FILE *file;
    char state;
    int res;

    file = fopen(path.c_str(), "r");
    if (!file)
        return false;
    res = fscanf(file, "%*d (%*[^)]) %c", &state);
    fclose(file);
    if (res != 1)
        return false;
    return state == 'Z';
}

pid_t TTask::GetPPid() const {
    std::string path = "/proc/" + std::to_string(Pid) + "/stat";
    int res, ppid;
    FILE *file;

    file = fopen(path.c_str(), "r");
    if (!file)
        return 0;
    res = fscanf(file, "%*d (%*[^)]) %*c %d", &ppid);
    fclose(file);
    if (res != 1)
        return 0;
    return ppid;
}

extern bool PostFork;
extern time_t ForkTime;
extern struct tm ForkLocalTime;
static std::mutex ForkLock;
static std::map<pid_t, TTask *> Tasks;
static std::condition_variable TasksCV;

// after that fork use only syscalls and signal-safe functions
TError TTask::Fork(bool detach) {
    PORTO_ASSERT(!PostFork);
    auto lock = std::unique_lock<std::mutex>(ForkLock);
    ForkTime = time(NULL);
    localtime_r(&ForkTime, &ForkLocalTime);
    pid_t ret = fork();
    if (ret < 0)
        return TError::System("TTask::Fork");
    Pid = ret;
    if (!Pid)
        PostFork = true;
    else if (!detach)
        Tasks[Pid] = this;
    Running = true;
    return OK;
}

TError TTask::Wait(bool interruptible,
                   const std::atomic_bool &stop,
                   const std::atomic_bool &disconnected) {
    TError error;
    auto lock = std::unique_lock<std::mutex>(ForkLock);
    if (Running) {
        pid_t pid = Pid;
        int status;
        lock.unlock();
        /* main thread could be blocked on lock that we're holding */
        pid_t pid_ = 0;
        while (true) {
            pid_ = waitpid(pid, &status, interruptible ? WNOHANG : 0);
            if (pid_)
                break;

            bool kill = stop || disconnected;
            if (kill) {
                auto killError = Kill(SIGKILL);
                if (killError)
                    L_ERR("Cannot kill helper: {}", killError);
                else {
                    if (stop) {
                        L("Kill helper on portod reload");
                        error = TError(EError::SocketError, "Helper killed by timeout on portod reload");
                    } else if (disconnected)
                        error = TError(EError::SocketError, "Helper killed at client disconnection");
                }
            }

            usleep(100 * 1000); // sleep 100 ms
        }

        if (pid_ == pid)
            pid = 0;

        lock.lock();
        if (!pid) {
            if (Running) {
                Tasks.erase(Pid);
                Running = false;
            }
            Status = status;
        }
    }
    while (Running) {
        if (kill(Pid, 0) && errno == ESRCH) {
            Tasks.erase(Pid);
            Running = false;
            Status = 100;
            return TError("task not found");
        }
        if (Tasks.find(Pid) == Tasks.end())
            return TError("detached task");
        TasksCV.wait(lock);
    }

    if (error)
        return error;

    if (Status)
        return TError(EError::Unknown, FormatExitStatus(Status));
    return OK;
}

bool TTask::Deliver(pid_t pid, int code, int status) {
    auto lock = std::unique_lock<std::mutex>(ForkLock);
    auto it = Tasks.find(pid);
    if (it == Tasks.end()) {
        /* already reaped and detached */
        if (kill(pid, 0) && errno == ESRCH)
            return true;
        return false;
    }
    it->second->Status = (code == CLD_EXITED) ? W_EXITCODE(status, 0) : status;
    it->second->Running = false;
    Tasks.erase(it);
    lock.unlock();
    TasksCV.notify_all();
    (void)waitpid(pid, NULL, 0);
    return true;
}

TError TranslatePid(pid_t pid, pid_t pidns, pid_t &result) {
    TUnixSocket sock, sk;
    TNamespaceFd pid_ns, mnt_ns, net_ns;
    TError error;
    TTask task;

    if (pidns <= 0 || pid == 0)
        return TError(EError::InvalidValue, "TranslatePid: invalid pid");
    if (pid > 0)
        result = prctl(PR_TRANSLATE_PID, pid, pidns, 0, 0);
    else
        result = prctl(PR_TRANSLATE_PID, -pid, 0, pidns, 0);
    if (result >= 0)
        return OK;
    if (errno == ESRCH)
        return TError(EError::InvalidValue, "TranslatePid: task not found");

    /* else fallback to SCM_CREDENTIALS */

    error = TUnixSocket::SocketPair(sock, sk);
    if (error)
        return error;

    error = pid_ns.Open(pidns, "ns/pid");
    if (error)
        return error;

    error = mnt_ns.Open(pidns, "ns/mnt");
    if (error)
        return error;

    error = net_ns.Open(pidns, "ns/net");
    if (error)
        return error;

    error = task.Fork();
    if (error)
        return error;

    if (task.Pid) {
        sk.Close();
        if (pid > 0) {
            error = sock.RecvPid(result, pid);
        } else {
            error = sock.SendPid(-pid);
            if (!error)
                error = sock.RecvPid(pid, result);
        }

        sock.Close();

        task.Wait();
        return error;
    }

    error = pid_ns.SetNs(CLONE_NEWPID);
    if (error)
        _exit(EXIT_FAILURE);

    error = mnt_ns.SetNs(CLONE_NEWNS);
    if (error)
        _exit(EXIT_FAILURE);

    error = net_ns.SetNs(CLONE_NEWNET);
    if (error)
        _exit(EXIT_FAILURE);

    TFile::CloseAllExcept({sk.GetFd()});

    pid_t child = vfork();
    if (child < 0)
        _exit(EXIT_FAILURE);

    if (!child) {
        if (pid > 0) {
            sk.SendPid(pid);
        } else {
            sk.RecvPid(result, pid);
            if (!result)
                sk.SendInt(0);
            else
                sk.SendPid(result);
        }
    } else
        waitpid(child, nullptr, 0);
    _exit(0);
}
