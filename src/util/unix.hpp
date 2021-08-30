#pragma once

#include "common.hpp"

#include <atomic>
#include <functional>
#include <vector>
#include <set>

#include "util/signal.hpp"
#include "util/path.hpp"

extern "C" {
#include <sys/resource.h>
#include <sys/syscall.h>
}

class TPath;

struct TTask {
    pid_t Pid = 0;
    int Status = 0;
    bool Running = false;

    TError Fork(bool detach = false);
    TError Wait(bool interruptible = false,
                const std::atomic_bool &stop = false,
                const std::atomic_bool &disconnected = false);
    static bool Deliver(pid_t pid, int status);

    bool Exists() const;
    bool IsZombie() const;
    pid_t GetPPid() const;
    TError Kill(int signal) const;
    TError KillPg(int signal) const;
};

std::string FormatTime(time_t t, const char *fmt = "%F %T");
void LocalTime(const time_t *time, struct tm &tm);

void TaintPostFork(std::string message);

pid_t GetPid();
pid_t GetPPid();
pid_t GetTid();
pid_t Clone(unsigned long flags, void *child_stack = NULL, void *ptid = NULL, void *ctid = NULL);
pid_t Fork(bool ptrace = false);
inline pid_t PtracedVfork() __attribute__((always_inline));
pid_t PtracedVfork() {
    pid_t pid = -1;

#ifdef __x86_64__
    __asm__ __volatile__ (
        "mov $" STRINGIFY(SYS_clone) ", %%rax;"
        "mov $" STRINGIFY(CLONE_VM | CLONE_VFORK | CLONE_PTRACE | SIGCHLD) ", %%rdi;"
        "mov $0, %%rsi;"
        "mov $0, %%rdx;"
        "mov $0, %%r10;"
        "mov $0, %%r8;"
        "syscall;"
        "mov %%eax, %0"
        : "=r" (pid)
        :
        : "rax", "rdi", "rsi", "rdx", "r10", "r8"
    );
#endif

    return pid;
}
TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens);
void PrintProc(const std::string &knob, pid_t pid, bool debug = true);
inline void PrintStack(pid_t pid, bool debug = true) {
    PrintProc("stack", pid, debug);
}

uint64_t GetCurrentTimeMs();
bool WaitDeadline(uint64_t deadline, uint64_t sleep = 10);
uint64_t GetTotalMemory();
uint64_t GetHugetlbMemory();
void SetProcessName(const std::string &name);
void SetDieOnParentExit(int sig);
void SetPtraceProtection(bool enable);
std::string GetTaskName(pid_t pid = 0);
uint64_t TaskHandledSignals(pid_t pid);
TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap);
std::string GetHostName();
TError SetHostName(const std::string &name);
TError GetSysctl(const std::string &name, std::string &value);
TError SetSysctl(const std::string &name, const std::string &value);
TError SetSysctlAt(const TFile &proc_sys, const std::string &name, const std::string &value);

TError SetOomScoreAdj(int value);

TError TranslatePid(pid_t pid, pid_t pidns, pid_t &result);

std::string FormatExitStatus(int status);
int GetNumCores();
void DumpMallocInfo();

int SetIoPrio(pid_t pid, int ioprio);

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
    std::string AltName;
    pid_t Pid = 0;

    TPidFile(const std::string &path, const std::string &name, const std::string &altname): Path(path), Name(name), AltName(altname) { }
    TError Read();
    bool Running();
    TError Save(pid_t pid);
    TError Remove();
};
