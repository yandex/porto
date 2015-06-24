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
int GetPid();
int GetPPid();
int GetTid();
size_t GetCurrentTimeMs();
size_t GetTotalMemory();
int CreatePidFile(const std::string &path, const int mode);
void RemovePidFile(const std::string &path);
void SetProcessName(const std::string &name);
void SetDieOnParentExit(int sig = SIGTERM);
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
    TScopedMem(size_t size);
    ~TScopedMem();
    void *GetData();
    size_t GetSize();
};

TError SetOomScoreAdj(int value);

int64_t GetBootTime();
TError Run(const std::vector<std::string> &command, int &status);
TError AllocLoop(const TPath &path, size_t size);
TError Popen(const std::string &cmd, std::vector<std::string> &lines);
TError PivotRoot(const TPath &rootfs);
size_t GetNumCores();
TError PackTarball(const TPath &tar, const TPath &path);
TError UnpackTarball(const TPath &tar, const TPath &path);
TError CopyRecursive(const TPath &src, const TPath &dst);
void DumpMallocInfo();
