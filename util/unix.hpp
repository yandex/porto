#ifndef __UNIX_HPP__
#define __UNIX_HPP__

#include <csignal>
#include <functional>

int RetryBusy(int times, int timeo_ms, std::function<int()> handler);
int RetryFailed(int times, int timeo_ms, std::function<int()> handler);
int SleepWhile(int timeo_ms, std::function<int()> handler);
int GetPid();
int RegisterSignal(int signum, void (*handler)(int));
void ResetAllSignalHandlers(void);

#endif
