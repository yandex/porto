#ifndef __CRASH_HPP__
#define __CRASH_HPP__

void WatchdogStart();
void WatchdogStrobe();

void Crash();
void SigsegvHandler(int sig, siginfo_t *si, void *unused);

#endif
