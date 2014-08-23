#include <iostream>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <csignal>

#include "porto.hpp"
#include "util/file.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
}

#define Expect(ret) _ExpectFailure(ret, true, 5, __LINE__, __func__)
#define ExpectSuccess(ret) _ExpectFailure(ret, 0, 5, __LINE__, __func__)
#define ExpectFailure(ret, exp) _ExpectFailure(ret, exp, 5, __LINE__, __func__)
static void _ExpectFailure(std::function<int()> f, int exp, int retry, int line, const char *func) {
    int ret;
    while (retry--) {
        ret = f();
        if (ret == exp)
            return;

        usleep(100000);
    }
    throw std::string("Got " + std::to_string(ret) + ", but expected " + std::to_string(exp) + " at " + func + ":" + std::to_string(line));
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
    Expect([&]{ std::cerr << "try" << std::endl; return true; });

    std::thread thr_tasks(Tasks);
    std::thread thr_stress_kill(StressKill);
    
    thr_tasks.join();
    thr_stress_kill.join();
    
    std::cout << "Test completed.\n";
    
    return 0;
}
