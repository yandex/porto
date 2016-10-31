#pragma once

#include "common.hpp"

#include <functional>
#include <vector>
#include <set>

#include "util/signal.hpp"
#include "util/path.hpp"

class TPath;

struct TTask {
    pid_t Pid = 0;
    int Status = 0;
    bool Running = false;

    TError Fork(bool detach = false);
    TError Wait();
    static bool Deliver(pid_t pid, int status);

    bool Exists() const;
    bool IsZombie() const;
    pid_t GetPPid() const;
    TError Kill(int signal) const;
};

std::string CurrentTimeFormat(const char *fmt, bool msec = false);

pid_t GetPid();
pid_t GetPPid();
pid_t GetTid();
TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens);

uint64_t GetCurrentTimeMs();
bool WaitDeadline(uint64_t deadline, uint64_t sleep = 10);
uint64_t GetTotalMemory();
void SetProcessName(const std::string &name);
void SetDieOnParentExit(int sig);
std::string GetTaskName(pid_t pid = 0);
TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap);
std::string GetHostName();
TError SetHostName(const std::string &name);
bool FdHasEvent(int fd);
TError SetSysctl(const std::string &name, const std::string &value);

TError SetOomScoreAdj(int value);

TError TranslatePid(pid_t pid, pid_t pidns, pid_t &result);

std::string FormatExitStatus(int status);
int GetNumCores();
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

class TPidFile {
public:
    TPath Path;
    std::string Name;
    pid_t Pid = 0;

    TPidFile(const std::string &path, const std::string &name): Path(path), Name(name) { }
    TError Load();
    bool Running();
    TError Save(pid_t pid);
    TError Remove();
};
