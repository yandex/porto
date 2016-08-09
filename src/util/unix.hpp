#pragma once

#include "common.hpp"

#include <functional>
#include <vector>
#include <set>

#include "util/signal.hpp"

class TPath;

struct TTask {
    pid_t Pid = 0;

    bool Exists() const;
    bool IsZombie() const;
    pid_t GetPPid() const;
    TError Kill(int signal) const;
};

pid_t ForkFromThread(void);
std::string CurrentTimeFormat(const char *fmt, bool msec = false);

pid_t GetPid();
pid_t GetPPid();
pid_t GetTid();
TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens);

uint64_t GetCurrentTimeMs();
bool WaitDeadline(uint64_t deadline, uint64_t sleep = 100);
size_t GetTotalMemory();
void SetProcessName(const std::string &name);
void SetDieOnParentExit(int sig);
std::string GetProcessName();
TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap);
std::string GetHostName();
TError SetHostName(const std::string &name);
bool FdHasEvent(int fd);
void CloseFds(int max, const std::set<int> &except, bool openStd = false);
TError ChattrFd(int fd, unsigned add_flags, unsigned del_flags);
TError SetSysctl(const std::string &name, const std::string &value);

class TScopedFd : public TNonCopyable {
    int Fd;
public:
    TScopedFd(int fd = -1);
    ~TScopedFd();
    int GetFd() const;
    TScopedFd &operator=(int fd);
};

TError SetOomScoreAdj(int value);

std::string FormatExitStatus(int status);
TError RunCommand(const std::vector<std::string> &command, const TPath &cwd);
TError Popen(const std::string &cmd, std::vector<std::string> &lines);
int GetNumCores();
TError PackTarball(const TPath &tar, const TPath &path);
TError UnpackTarball(const TPath &tar, const TPath &path);
TError CopyRecursive(const TPath &src, const TPath &dst);
void DumpMallocInfo();

class TUnixSocket : public TNonCopyable {
    int SockFd;
public:
    static TError SocketPair(TUnixSocket &sock1, TUnixSocket &sock2);
    TUnixSocket(int sock = -1) : SockFd(sock) {};
    ~TUnixSocket() { Close(); };
    void operator=(int sock);
    TUnixSocket& operator=(TUnixSocket&& Sock);
    void Close();
    int GetFd() const { return SockFd; }
    TError SendInt(int val) const;
    TError RecvInt(int &val) const;
    TError SendZero() const { return SendInt(0); }
    TError RecvZero() const { int zero; return RecvInt(zero); }
    TError SendPid(pid_t pid) const;
    TError RecvPid(pid_t &pid, pid_t &vpid) const;
    TError SendError(const TError &error) const;
    TError RecvError() const;
    TError SendFd(int fd) const;
    TError RecvFd(int &fd) const;
    TError SetRecvTimeout(int timeout_ms) const;
};
