#ifndef __UNIX_HPP__
#define __UNIX_HPP__

#include <functional>

int RetryBusy(int times, int timeo_ms, std::function<int()> handler);
int RetryFailed(int times, int timeo_ms, std::function<int()> handler);
void SleepWhile(int timeo_ms, std::function<bool()> handler);
int GetPid();
int RegisterSignal(int signum, void (*handler)(int));

#endif
