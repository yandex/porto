#ifndef __UNIX_HPP__
#define __UNIX_HPP__

#include <csignal>
#include <functional>

int RetryBusy(int times, int timeoMs, std::function<int()> handler);
int RetryFailed(int times, int timeoMs, std::function<int()> handler);
int SleepWhile(int timeoMs, std::function<int()> handler);
int GetPid();
size_t GetCurrentTimeMs();
int RegisterSignal(int signum, void (*handler)(int));
void ResetAllSignalHandlers(void);
std::string DirName(const std::string &str);
size_t GetTotalMemory();
std::string GetDefaultUser();
std::string GetDefaultGroup();
int CreatePidFile(const std::string &path, const int mode);
void RemovePidFile(const std::string &path);

#endif
