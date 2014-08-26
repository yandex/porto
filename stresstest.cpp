#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>
#include <map>
#include <algorithm>

#include "porto.hpp"
#include "util/file.hpp"

#include "rpc.pb.h"
#include "libporto.hpp"
#include "util/file.hpp"
#include "util/string.hpp"

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
        usleep(1000000);
        std::cout << "Retry " << func << ":" << line << " Ret=" << ret << std::endl;
    }
    throw std::string("Got " + std::to_string(ret) + ", but expected " + std::to_string(exp) + " at " + func + ":" + std::to_string(line));
}

static std::atomic<int> done;

static std::vector<std::map<std::string, std::string>> vtasks =
{
    {
        {"command", "bash -c 'sleep $N'"},
        {"env", "N=1"},
        {"stdout", ""},
        {"stderr", ""},
        {"exit_status", "0"}
    },
    {
        {"command", "bash -c 'echo $A'"},
        {"env", "A=qwerty"},
        {"stdout", "qwerty"},
        {"stderr", ""},
        {"exit_status", "0"}
    }
};

static bool TaskRunning(TPortoAPI &api, const std::string &pid, const std::string &name) {
    int p = stoi(pid);
    std::string ret;
    
    (void)api.GetData(name, "state", ret);
        return kill(p, 0) == 0;
}

static void Create(std::string name) {
    TPortoAPI api;
    std::vector<std::string> containers;
    
    Expect([&]{ containers.clear(); api.List(containers); return std::find(containers.begin(),containers.end(),name) == containers.end();});
    ExpectSuccess([&]{return api.Create(name);});
    Expect([&]{ containers.clear(); api.List(containers); return std::find(containers.begin(),containers.end(),name) != containers.end();});
}

static void SetProperty(std::string name, std::string type, std::string value) {
    TPortoAPI api;
    std::string res_value;
    
    ExpectSuccess([&]{return api.SetProperty(name, type, value);});
    ExpectSuccess([&]{return api.GetProperty(name, type, res_value);});
    Expect([&]{return res_value==value;});
}

static void Start(std::string name) {
    TPortoAPI api;
    std::string pid;
    std::string v;
    
    ExpectSuccess([&]{return api.Start(name);});
    Expect([&]{ api.GetData(name, "state", v); return v == "dead" || v == "running"; });
}

static void Destroy(std::string name) {
    TPortoAPI api;
    std::vector<std::string> containers;
    
    ExpectSuccess([&]{return api.List(containers);});
    Expect([&]{return std::find(containers.begin(),containers.end(),name) != containers.end();});
    ExpectSuccess([&]{return api.Destroy(name);});
    containers.clear();
    ExpectSuccess([&]{return api.List(containers);});
    Expect([&]{return std::find(containers.begin(),containers.end(),name) == containers.end();});
    
}

static void Tasks() {
    try {
        TPortoAPI api;
        int i = 1000;
        while (i--) {
        for (unsigned int t=0; t < vtasks.size(); t++) {
            std::string name = "stresstest" + std::to_string(t);
        
            std::cout << "Create container stresstest" << t << std::endl;
            Create(name);
    
            SetProperty(name, "env", vtasks[0]["env"]);
        
            SetProperty(name, "command", vtasks[0]["command"]);
        
            Start(name);
        
            std::cout << "Destroy container stresstest" << t << std::endl;
            Destroy(name);
        }
        }
    } catch (std::string e) {
        std::cerr << "Exception " << e << std::endl;
        done++;
        return;
    }
    done++;
    std::cout << "Tasks completed." << std::endl;
}

static void StressKill() {
    TPortoAPI api;

    while (!done) {
        //usleep(rand() % 2000000);
        usleep(1000000);
        TFile f(PID_FILE);
        int pid;
        std::vector<std::string> containers;
        if (api.List(containers) != 0)
            continue;
        if (f.AsInt(pid))
            std::cerr << "Don't open " << PID_FILE << std::endl;
        if (kill(pid, SIGKILL)) {
            std::cerr << "Don't send kill to " << pid << std::endl;
        } else {
            std::cout << "Killed " << pid << std::endl;
        }
    }
}

int StressTest() {
    try {
        (void)signal(SIGPIPE, SIG_IGN);
        Expect([&]{ std::cerr << "try" << std::endl; return true; });

        std::thread thr_tasks(Tasks);
        std::thread thr_stress_kill(StressKill);
    
        thr_tasks.join();
        thr_stress_kill.join();
    } catch (std::string e) {
        std::cerr << "Exception " << e << std::endl;
        return 1;
    }
    std::cout << "Test completed." << std::endl;
    
    return 0;
}
