#pragma once

#include "common.hpp"

#include <functional>
#include <vector>
#include <set>

#include "util/signal.hpp"

class TPath;

bool RetryIfFailed(std::function<int()> handler, int &ret, int times = 10, int timeoMs = 100);
bool SleepWhile(std::function<int()> handler, int &ret, int timeoMs);

pid_t GetPid();
pid_t GetPPid();
pid_t GetTid();
TError GetTaskParent(pid_t pid, pid_t &parent_pid);
TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens);

uint64_t GetCurrentTimeMs();
size_t GetTotalMemory();
int CreatePidFile(const std::string &path, const int mode);
void SetProcessName(const std::string &name);
void SetDieOnParentExit(int sig);
std::string GetProcessName();
TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap);
std::string GetHostName();
TError SetHostName(const std::string &name);
bool FdHasEvent(int fd);
TError DropBoundedCap(int cap);
TError SetCap(uint64_t effective, uint64_t permitted, uint64_t inheritable);
void CloseFds(int max, const std::set<int> &except, bool openStd = false);
TError ChattrFd(int fd, unsigned add_flags, unsigned del_flags);

class TScopedFd : public TNonCopyable {
    int Fd;
public:
    TScopedFd(int fd = -1);
    ~TScopedFd();
    int GetFd() const;
    TScopedFd &operator=(int fd);
};

TError SetOomScoreAdj(int value);

TError Run(const std::vector<std::string> &command, int &status, bool stdio = false);
TError AllocLoop(const TPath &path, size_t size);
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
};
