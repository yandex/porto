#pragma once

#include <csignal>

int RegisterSignal(int signum, void (*handler)(int));
int RegisterSignal(int signum, void (*handler)(int sig, siginfo_t *si, void *unused));
void ResetSignalHandler(int signum);
void ResetAllSignalHandlers(void);
void RaiseSignal(int signum);
