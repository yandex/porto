#include <iostream>
#include <thread>
#include <atomic>
#include <unistd.h>

#include "porto.hpp"
#include "util/file.hpp"

extern "C" {
    #include <unistd.h>
    #include <sys/types.h>
    #include <signal.h>
}

static std::atomic<int> done;

static void Tasks() {
    usleep(60000000);
    done++;
    std::cout << "Tasks completed.\n";
}

static void StressKill() {
    while (!done) {
        usleep(rand() % 2000000);
        TFile f(PID_FILE);
        int pid;
        if (f.AsInt(pid))
            std::cerr << "Don't open " << PID_FILE << "\n";
        if (kill(pid, SIGKILL)) {
            std::cerr << "Don't send kill to " << pid << "\n";
        } else {
            std::cout << "Killed " << pid << "\n";
        }
    }
}

int StressTest() {
    std::thread thr_tasks(Tasks);
    std::thread thr_stress_kill(StressKill);
    
    thr_tasks.join();
    thr_stress_kill.join();
    
    std::cout << "Test completed.\n";
    
    return 0;
}
