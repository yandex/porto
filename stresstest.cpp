#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include "util/file.hpp"

extern "C" {
    #include <unistd.h>
    #include <sys/types.h>
    #include <signal.h>
}

static const std::string PID_FILE = "/run/portod.pid";

static std::atomic<int> readers;
static std::mutex write_lock;
static bool run_status = true;

static void Changerunstatus(bool status) {
    write_lock.lock();
    while(readers > 0);
    run_status = status;
    write_lock.unlock();
}

static bool Runstatus() {
    bool res;
    write_lock.lock();
    readers++;
    write_lock.unlock();
    res = run_status;
    readers--;
    return res;
}

static void Tasks() {
    usleep(60000000);
    Changerunstatus(false);
    std::cout << "Tasks completed.\n";
}

static void Stresskill() {
    while(Runstatus()) {
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

int Stresstest() {
    std::thread thr_tasks(Tasks);
    std::thread thr_stress_kill(Stresskill);
    
    thr_tasks.join();
    thr_stress_kill.join();
    
    std::cout << "Test completed.\n";
    
    return 0;
}
