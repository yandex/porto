#pragma once

#include <csignal>
#include <functional>

#include "common.hpp"

extern "C" {
#include <signal.h>
#include <sys/epoll.h>
}

constexpr int updateSignal = SIGHUP;
constexpr int rotateSignal = SIGUSR1;

static constexpr int HANDLE_SIGNALS[] = {SIGINT, SIGTERM,
                                         updateSignal, rotateSignal,
                                         SIGALRM};
static constexpr int HANDLE_SIGNALS_WAIT[] = {SIGCHLD};

class TPath;

int RetryBusy(int times, int timeoMs, std::function<int()> handler);
int RetryFailed(int times, int timeoMs, std::function<int()> handler);
int SleepWhile(int timeoMs, std::function<int()> handler);
int GetPid();
size_t GetCurrentTimeMs();
int RegisterSignal(int signum, void (*handler)(int));
int RegisterSignal(int signum, void (*handler)(int sig, siginfo_t *si, void *unused));
void ResetAllSignalHandlers(void);
void RaiseSignal(int signum);
TError InitializeSignals(int &SignalFd, int Epfd);
TError ReadSignalFd(int fd, std::vector<int> &signals);
size_t GetTotalMemory();
int CreatePidFile(const std::string &path, const int mode);
void RemovePidFile(const std::string &path);
void SetProcessName(const std::string &name);
void SetDieOnParentExit();
std::string GetProcessName();
TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap);
int BlockAllSignals();
std::string GetHostName();
TError SetHostName(const std::string &name);
bool FdHasEvent(int fd);
TError DropBoundedCap(int cap);
TError SetCap(uint64_t effective, uint64_t permitted, uint64_t inheritable);
void CloseAllFds();

class TScopedFd : public TNonCopyable {
    int Fd;
public:
    TScopedFd(int fd = -1);
    ~TScopedFd();
    int GetFd();
    TScopedFd &operator=(int fd);
};

TError SetOomScoreAdj(int value);

TError EpollCreate(int &epfd);
TError EpollAdd(int &epfd, int fd);

int64_t GetBootTime();
TError Run(const std::vector<std::string> &command, int &status);
TError AllocLoop(const TPath &path, size_t size);
TError Popen(const std::string &cmd, std::vector<std::string> &lines);

class TEpollLoop : public TNonCopyable {
public:
    TError Create();

    TError AddFd(int fd);
    TError GetEvents(std::vector<int> &signals,
                     std::vector<struct epoll_event> &events,
                     int timeout);

private:
    TError InitializeSignals();
    bool GetSignals(std::vector<int> &signals);

    int EpollFd;

    static const int MAX_EVENTS = 32;
    struct epoll_event Events[MAX_EVENTS];
};
