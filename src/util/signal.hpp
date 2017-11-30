#pragma once

extern "C" {
#include <signal.h>
#include <sys/signalfd.h>
}

void Crash();
void FatalSignal(int sig);
void CatchFatalSignals();
void ResetBlockedSignals();
void ResetIgnoredSignals();

void Signal(int signum, void (*handler)(int));
int SignalFd();
