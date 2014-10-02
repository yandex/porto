#ifndef __CRASH_HPP__
#define __CRASH_HPP__

void WatchdogStart(int maxFails, int delayS);
void WatchdogStrobe();

void Crash();

#endif
