#ifndef __UNIX_HPP__
#define __UNIX_HPP__

#include <functional>

int RetryBusy(int times, int timeo, std::function<int()> handler);
int RetryFailed(int times, int timeo, std::function<int()> handler);

#endif
