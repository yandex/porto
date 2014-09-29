#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>
#include <map>
#include <algorithm>

#include "porto.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "test.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
}

namespace test {

static const int retries = 10;
static std::atomic<int> fail;

static std::vector<std::map<std::string, std::string>> vtasks =
{
    {
        { "command", "bash -ec 'sleep $N'" },
        { "env", "N=1" },
        { "stdout", "" },
        { "stderr", "" },
        { "exit_status", "0" },
        { "timeout", "5" },
    },
    {
        { "command", "bash -ec 'echo $A'" },
        { "env", "A=qwerty" },
        { "stdout", "qwerty\n" },
        { "stderr", "" },
        { "exit_status", "0" },
        { "timeout", "5" },
    },
    {
        { "command", "bash -ec 'echo $A && false'" },
        { "env", "A=qwerty" },
        { "stdout", "qwerty\n" },
        { "stderr", "" },
        { "exit_status", "256" },
        { "timeout", "5" },
    },
    {
        { "command", "bash -ec 'for i in $A; do sleep 1; echo $i >&2; done'" },
        { "env", "A=1 2 3" },
        { "stdout", "" },
        { "stderr", "1\n2\n3\n" },
        { "exit_status", "0" },
        { "timeout", "10" },
    }
};

static void Create(const std::string &name, const std::string &cwd) {
    TPortoAPI api(retries);
    std::vector<std::string> containers;

    Say() << "Create container: " << name << std::endl;

    containers.clear();
    api.List(containers);
    Expect(std::find(containers.begin(),containers.end(),name) == containers.end());
    auto ret = api.Create(name);
    Expect(ret == EError::Success || ret == EError::ContainerAlreadyExists);
    containers.clear();
    api.List(containers);
    Expect(std::find(containers.begin(),containers.end(),name) != containers.end());

    TFolder f(cwd);
    if (!f.Exists()) {
        TError error = f.Create(0755, true);
        Expect(error == false);
    }
}

static void SetProperty(std::string name, std::string type, std::string value) {
    TPortoAPI api(retries);
    std::string res_value;

    Say() << "SetProperty container: " << name << std::endl;

    ExpectSuccess(api.SetProperty(name, type, value));
    ExpectSuccess(api.GetProperty(name, type, res_value));
    Expect(res_value == value);
}

static void Start(std::string name) {
    TPortoAPI api(retries);
    std::string pid;
    std::string ret;

    Say() << "Start container: " << name << std::endl;

    ExpectSuccess(api.Start(name));
    api.GetData(name, "state", ret);
    Expect(ret == "dead" || ret == "running");
}

static void CheckRuning(std::string name, std::string timeout) {
    TPortoAPI api(retries);
    std::string pid;
    std::string ret;
    int t;

    Say() << "CheckRuning container: " << name << std::endl;

    StringToInt(timeout, t);
    while (t--) {
        api.GetData(name, "state", ret);
        Say() << "Poll " << name << ": "<< ret << std::endl;
        if (ret == "dead")
            return;
        usleep(1000000);
    }
    done++;
    throw std::string("Timeout");
}

static void CheckStdout(std::string name, std::string stream) {
    TPortoAPI api(retries);
    std::string ret;

    Say() << "CheckStdout container: " << name << std::endl;

    api.GetData(name, "stdout", ret);
    Expect(ret == stream);
}

static void CheckStderr(std::string name, std::string stream) {
    TPortoAPI api(retries);
    std::string ret;

    Say() << "CheckStderr container: " << name << std::endl;

    api.GetData(name, "stderr", ret);
    Expect(ret == stream);
}

static void CheckExit(std::string name, std::string stream) {
    TPortoAPI api(retries);
    std::string ret;
    Say() << "CheckExit container: " << name << std::endl;
    api.GetData(name, "exit_status", ret);
    Expect(ret == stream);
}

static void Destroy(const std::string &name, const std::string &cwd) {
    TPortoAPI api(retries);
    std::vector<std::string> containers;

    Say() << "Destroy container: " << name << std::endl;

    ExpectSuccess(api.List(containers));
    Expect(std::find(containers.begin(),containers.end(),name) != containers.end());
    ExpectSuccess(api.Destroy(name));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(std::find(containers.begin(),containers.end(),name) == containers.end());
    TFolder f(cwd);
    f.Remove(true);
}

static void Tasks(int n, int iter) {
    tid = n;
    Say() << "Run task" << std::to_string(n) << std::endl;
    usleep(10000 * n);
    try {
        TPortoAPI api(retries);
        while (iter--) {
            for (unsigned int t = 0; t < vtasks.size(); t++) {
                std::string name = "stresstest" + std::to_string(n) + "_" + std::to_string(t);
                std::string cwd = "/tmp/stresstest/" + name;
                Create(name, cwd);
                SetProperty(name, "env", vtasks[t]["env"]);
                SetProperty(name, "command", vtasks[t]["command"]);
                SetProperty(name, "cwd", cwd);
                Start(name);
                CheckRuning(name, vtasks[t]["timeout"]);
                CheckExit(name, vtasks[t]["exit_status"]);
                CheckStdout(name, vtasks[t]["stdout"]);
                CheckStderr(name, vtasks[t]["stderr"]);
                Destroy(name, cwd);
            }
        }
    } catch (std::string e) {
        Say() << "ERROR: Exception " << e << std::endl;
        Say() << "ERROR: Stop task" << std::to_string(n) << std::endl;
        fail++;
        return;
    }
    Say() << "Stop task" << std::to_string(n) << std::endl;
}

static void StressKill() {
    TPortoAPI api(retries);
    std::cout << "Run kill" << std::endl;
    while (!done) {
        usleep(1000000);
        TFile f(PID_FILE);
        int pid;
        std::vector<std::string> containers;
        if (api.List(containers) != 0)
            continue;
        if (f.AsInt(pid))
            Say(std::cerr) << "ERROR: Don't open " << PID_FILE << std::endl;
        if (kill(pid, SIGKILL)) {
            Say(std::cerr) << "ERROR: Don't send kill to " << pid << std::endl;
        } else {
            std::cout << "Killed " << pid << std::endl;
        }
    }
}

int StressTest(int threads, int iter, bool killPorto) {
    int i;
    std::vector<std::thread> thrTasks;
    std::thread thrKill;

    try {
        (void)signal(SIGPIPE, SIG_IGN);

        TPortoAPI api(retries);
        RestartDaemon(api);

        for (i = 1; i <= threads; i++)
            thrTasks.push_back(std::thread(Tasks, i, iter));
        if (killPorto)
            thrKill = std::thread(StressKill);
        for (auto& th : thrTasks)
            th.join();
        done++;
        if (killPorto)
            thrKill.join();

        TestDaemon(api);
    } catch (std::string e) {
        std::cerr << "ERROR: Exception " << e << std::endl;
        fail++;
    }

    if (!fail)
        std::cout << "Test completed!" << std::endl;

    return 0;
}
}
