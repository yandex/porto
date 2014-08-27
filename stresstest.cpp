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

static const int retries = 10;

#define Expect(ret) _ExpectFailure(ret, true, retries, __LINE__, __func__)
#define ExpectSuccess(ret) _ExpectFailure(ret, 0, retries, __LINE__, __func__)
#define ExpectFailure(ret, exp) _ExpectFailure(ret, exp, retries, __LINE__, __func__)

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

static std::atomic<int> fail;
static std::atomic<int> done;

static std::vector<std::map<std::string, std::string>> vtasks =
{
    {
        {"command", "bash -ec 'sleep $N'"},
        {"env", "N=1"},
        {"stdout", ""},
        {"stderr", ""},
        {"exit_status", "0"},
        {"timeout", "5"}
    },
    {
        {"command", "bash -ec 'echo $A'"},
        {"env", "A=qwerty"},
        {"stdout", "qwerty\n"},
        {"stderr", ""},
        {"exit_status", "0"},
        {"timeout", "5"}
    },
    {
        {"command", "bash -ec 'echo $A && false'"},
        {"env", "A=qwerty"},
        {"stdout", "qwerty\n"},
        {"stderr", ""},
        {"exit_status", "1"},
        {"timeout", "5"}
    },
    {
        {"command", "bash -ec 'for i in $A; do sleep 1; echo $i >&2; done'"},
        {"env", "A=1 2 3"},
        {"stdout", ""},
        {"stderr", "1\n2\n3\n"},
        {"exit_status", "0"},
        {"timeout", "5"}
    }
};

static void Create(std::string name) {
    TPortoAPI api;
    std::vector<std::string> containers;
    
    std::cout << "Create container: " << name << std::endl;
    
    Expect([&]{ containers.clear(); api.List(containers); return std::find(containers.begin(),containers.end(),name) == containers.end();});
    Expect([&]{ auto ret = api.Create(name); return ret == EError::Success || ret == EError::ContainerAlreadyExists; });
    Expect([&]{ containers.clear(); api.List(containers); return std::find(containers.begin(),containers.end(),name) != containers.end();});
}

static void SetProperty(std::string name, std::string type, std::string value) {
    TPortoAPI api;
    std::string res_value;
    
    std::cout << "SetProperty container: " << name << std::endl;
    
    ExpectSuccess([&]{return api.SetProperty(name, type, value);});
    ExpectSuccess([&]{return api.GetProperty(name, type, res_value);});
    Expect([&]{return res_value==value;});
}

static void Start(std::string name) {
    TPortoAPI api;
    std::string pid;
    std::string ret;
    
    std::cout << "Start container: " << name << std::endl;
    
    ExpectSuccess([&]{return api.Start(name);});
    Expect([&]{ api.GetData(name, "state", ret); return ret == "dead" || ret == "running";});
}

static void CheckRuning(std::string name, std::string timeout) {
    TPortoAPI api;
    std::string pid;
    std::string ret;
    int t;
    
    std::cout << "CheckRuning container: " << name << std::endl;
    
    StringToInt(timeout, t);
    while (t--) {
        api.GetData(name, "state", ret);
        std::cout << "Poll " << name << ": "<< ret << std::endl;
        if (ret == "dead") {
            return;
        }
        usleep(1000000);
    }
    fail++;
    done++;
    throw std::string("Timeout");
}

static void CheckStdout(std::string name, std::string stream) {
    TPortoAPI api;
    std::string ret;
    
    std::cout << "CheckStdout container: " << name << std::endl;
    
    Expect([&]{api.GetData(name, "stdout", ret); return ret == stream;});
}

static void CheckStderr(std::string name, std::string stream) {
    TPortoAPI api;
    std::string ret;
    
    std::cout << "CheckStderr container: " << name << std::endl;
    
    Expect([&]{api.GetData(name, "stderr", ret); return ret == stream;});
}

static void CheckExit(std::string name, std::string stream) {
    TPortoAPI api;
    std::string ret;
    int exit_ret, exit_stream;
    std::cout << "CheckExit container: " << name << std::endl;
    
    Expect([&]{api.GetData(name, "exit_status", ret); StringToInt(ret, exit_ret); StringToInt(stream, exit_stream); return WEXITSTATUS(exit_ret) == exit_stream;});
}

static void Destroy(std::string name) {
    TPortoAPI api;
    std::vector<std::string> containers;
    
    std::cout << "Destroy container: " << name << std::endl;
    
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
                Create(name);
                SetProperty(name, "env", vtasks[t]["env"]);
                SetProperty(name, "command", vtasks[t]["command"]);
                Start(name);
                CheckRuning(name, vtasks[t]["timeout"]);
                CheckStdout(name, vtasks[t]["stdout"]);
                CheckStderr(name, vtasks[t]["stderr"]);
                CheckExit(name, vtasks[t]["exit_status"]);
                Destroy(name);
            }
        }
    } catch (std::string e) {
        std::cerr << "ERROR: Exception " << e << std::endl;
        fail++;
        done++;
        return;
    }
    done++;
}

static void StressKill() {
    TPortoAPI api;

    while (!done) {
        usleep(1000000);
        TFile f(PID_FILE);
        int pid;
        std::vector<std::string> containers;
        if (api.List(containers) != 0)
            continue;
        if (f.AsInt(pid))
            std::cerr << "ERROR: Don't open " << PID_FILE << std::endl;
        if (kill(pid, SIGKILL)) {
            std::cerr << "ERROR: Don't send kill to " << pid << std::endl;
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
        std::cerr << "ERROR: Exception " << e << std::endl;
        return 1;
    }
    if (fail) {
        std::cerr << "ERROR: Test fail!!!" << std::endl;
        return 1;
    }
    std::cout << "Test completed!" << std::endl;
    return 0;
}
