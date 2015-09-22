#pragma once

#include <functional>
#include <vector>
#include <set>

#include "common.hpp"
#include "util/signal.hpp"

class TPath;

int RetryBusy(int times, int timeoMs, std::function<int()> handler);
int RetryFailed(int times, int timeoMs, std::function<int()> handler);
int SleepWhile(int timeoMs, std::function<int()> handler);

pid_t GetPid();
pid_t GetPPid();
pid_t GetTid();
TError GetTaskParent(pid_t pid, pid_t &parent_pid);
TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens);

size_t GetCurrentTimeMs();
size_t GetTotalMemory();
int CreatePidFile(const std::string &path, const int mode);
void RemovePidFile(const std::string &path);
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

class TScopedFd : public TNonCopyable {
    int Fd;
public:
    TScopedFd(int fd = -1);
    ~TScopedFd();
    int GetFd() const;
    TScopedFd &operator=(int fd);
};

class TScopedMem : public TNonCopyable {
    size_t Size;
    void *Data = nullptr;
public:
    TScopedMem();
    TScopedMem(size_t size);
    ~TScopedMem();
    void Alloc(size_t size);
    void Free();
    void *GetData();
    size_t GetSize();
};

TError SetOomScoreAdj(int value);

int64_t GetBootTime();
TError Run(const std::vector<std::string> &command, int &status, bool stdio = false);
TError AllocLoop(const TPath &path, size_t size);
TError Popen(const std::string &cmd, std::vector<std::string> &lines);
TError PivotRoot(const TPath &rootfs);
size_t GetNumCores();
TError PackTarball(const TPath &tar, const TPath &path);
TError UnpackTarball(const TPath &tar, const TPath &path);
TError CopyRecursive(const TPath &src, const TPath &dst);
void DumpMallocInfo();
std::string GetCwd();

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
};
