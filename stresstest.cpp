#include <iostream>
#include <thread>
#include <unistd.h>
#include "util/file.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
}

static const std::string PID_FILE = "/run/portod.pid";

static void PLan() {
    usleep(60000000);
}

static void Kill() {
    for (;;) {
        usleep(rand() % 2000000);
        TFile f(PID_FILE);
        int pid;
        if (f.AsInt(pid))
            std::cerr << "Don't open " << PID_FILE << "\n";
        if (kill(pid, SIGKILL))
            std::cerr << "Don't send kill to " << pid << "\n";
    }
}

int Stresstest() {
    std::thread thr_plan (PLan);
    std::thread thr_kill (Kill);
    
    thr_plan.join();
    
    thr_kill.detach();
    
    std::cout << "Test completed.\n";
    
    return 0;
}
