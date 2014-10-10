#ifndef __UNIX_HPP__
#define __UNIX_HPP__

#include <csignal>
#include <functional>

#include "porto.hpp"
#include "error.hpp"

int RetryBusy(int times, int timeoMs, std::function<int()> handler);
int RetryFailed(int times, int timeoMs, std::function<int()> handler);
int SleepWhile(int timeoMs, std::function<int()> handler);
int GetPid();
size_t GetCurrentTimeMs();
int RegisterSignal(int signum, void (*handler)(int));
int RegisterSignal(int signum, void (*handler)(int sig, siginfo_t *si, void *unused));
void ResetAllSignalHandlers(void);
std::string DirName(const std::string &str);
std::string BaseName(const std::string &str);
size_t GetTotalMemory();
int CreatePidFile(const std::string &path, const int mode);
void RemovePidFile(const std::string &path);
void SetProcessName(const std::string &name);
TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap);
int BlockAllSignals();

class TScopedFd {
    NO_COPY_CONSTRUCT(TScopedFd);
    int Fd;
public:
    TScopedFd(int fd = -1);
    ~TScopedFd();
    int GetFd();
    TScopedFd &operator=(int fd);
};

#endif
