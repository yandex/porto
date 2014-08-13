#include <iostream>
#include <iomanip>
#include <sstream>

#include "rpc.pb.h"
#include "libporto.hpp"
#include "file.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
}

using namespace std;

#define Expect(ret) _ExpectFailure(ret, true, __LINE__, __func__)
#define ExpectSuccess(ret) _ExpectFailure(ret, 0, __LINE__, __func__)
#define ExpectFailure(ret, exp) _ExpectFailure(ret, exp, __LINE__, __func__)
static void _ExpectFailure(int ret, int exp, int line, const char *func) {
    if (ret != exp)
        throw string(to_string(ret) + " != " + to_string(exp) + " at " + func + ":" + to_string(line));
}

static void ShouldHaveOnlyRoot(TPortoAPI &api) {
    std::vector<std::string> containers;

    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 1);
    Expect(containers[0] == string("/"));
}

static void ShouldHaveValidProperties(TPortoAPI &api, const string &name) {
    string v;

    ExpectSuccess(api.GetProperty(name, "command", v));
    Expect(v == string(""));
    ExpectSuccess(api.GetProperty(name, "low_limit", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "user", v));
    Expect(v == string("nobody"));
    ExpectSuccess(api.GetProperty(name, "group", v));
    Expect(v == string("nogroup"));
    ExpectSuccess(api.GetProperty(name, "env", v));
    Expect(v == string(""));
}

static void ShouldHaveValidData(TPortoAPI &api, const string &name) {
    string v;

    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == string("stopped"));
    ExpectSuccess(api.GetData(name, "exit_status", v));
    Expect(v == string("-1"));
    ExpectSuccess(api.GetData(name, "root_pid", v));
    Expect(v == string("-1"));
    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v == string(""));
    ExpectSuccess(api.GetData(name, "stderr", v));
    Expect(v == string(""));
    ExpectSuccess(api.GetData(name, "cpu_usage", v));
    Expect(v == string("-1"));
    ExpectSuccess(api.GetData(name, "mem_usage", v));
    Expect(v == string("-1"));
}

static void TestHolder(TPortoAPI &api) {
    ShouldHaveOnlyRoot(api);

    std::vector<std::string> containers;

    cerr << "Create container A" << endl;
    ExpectSuccess(api.Create("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    cerr << "Try to create existing container A" << endl;
    ExpectFailure(api.Create("a"), rpc::EContainerError::ContainerAlreadyExists);
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    cerr << "Create container B" << endl;
    ExpectSuccess(api.Create("b"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("b"));
    ShouldHaveValidProperties(api, "b");
    ShouldHaveValidData(api, "b");

    cerr << "Remove container A" << endl;
    ExpectSuccess(api.Destroy("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("b"));

    cerr << "Remove container B" << endl;
    ExpectSuccess(api.Destroy("b"));

    cerr << "Try to execute operations on invalid container" << endl;
    ExpectFailure(api.Start("a"), rpc::EContainerError::ContainerDoesNotExist);
    ExpectFailure(api.Stop("a"), rpc::EContainerError::ContainerDoesNotExist);
    ExpectFailure(api.Pause("a"), rpc::EContainerError::ContainerDoesNotExist);
    ExpectFailure(api.Resume("a"), rpc::EContainerError::ContainerDoesNotExist);

    string value;
    ExpectFailure(api.GetProperty("a", "command", value), rpc::EContainerError::ContainerDoesNotExist);
    ExpectFailure(api.SetProperty("a", "command", value), rpc::EContainerError::ContainerDoesNotExist);
    ExpectFailure(api.GetData("a", "root_pid", value), rpc::EContainerError::ContainerDoesNotExist);

    ShouldHaveOnlyRoot(api);
}

static void TestEmpty(TPortoAPI &api) {
    cerr << "Make sure we can't start empty container" << endl;
    ExpectSuccess(api.Create("b"));
    ExpectFailure(api.Start("b"), rpc::EContainerError::Error);
    ExpectSuccess(api.Destroy("b"));
}

static bool TaskRunning(TPortoAPI &api, const string &pid, const string &name) {
    int p = stoi(pid);

    string ret;
    (void)api.GetData(name, "exit_status", ret);

    return kill(p, 0) == 0;
}

static void WaitPid(TPortoAPI &api, const string &pid, const string &name) {
    cerr << "Waiting for " << pid << " to exit..." << endl;

    int times = 100;
    int p = stoi(pid);

    do {
        if (times-- <= 0)
            break;

        usleep(100000);

        /* we need to reap our child process which happens in the
         * exit_status handler, so execute it periodically */
        string ret;
        (void)api.GetData(name, "exit_status", ret);

        kill(p, 0);
    } while (errno != ESRCH);

    if (times <= 0)
        throw string("Waited too long for task to exit");
}

static void TestExitStatus(TPortoAPI &api, const string &name) {
    string pid;
    string ret;

    cerr << "Check exit status of 'false'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "false"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("0 0 1"));

    cerr << "Check exit status of 'true'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("0 0 0"));

    cerr << "Check exit status of invalid command" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectFailure(api.Start(name), rpc::EContainerError::Error);
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    // TODO: this may blow things up inside portod
    Expect(pid == "0");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("2 0 0"));
}

static void TestStreams(TPortoAPI &api, const string &name) {
    string pid;
    string ret;

    cerr << "Make sure stdout works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("out\n"));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string(""));

    cerr << "Make sure stderr works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string("err\n"));
}

static string GetNamespace(const string &pid, const string &ns) {
    TError error;
    string link;
    TFile m("/proc/" + pid + "/ns/" + ns);
    (void)m.ReadLink(link);
    return link;
}

static map<string, string> GetCgroups(const string &pid) {
    TFile f("/proc/" + pid + "/cgroup");
    vector<string> lines;
    map<string, string> cgmap;
    (void)f.AsLines(lines);

    vector<string> tokens;
    for (auto l : lines) {
        tokens.clear();
        (void)SplitString(l, ':', tokens);
        cgmap[tokens[1]] = tokens[2];
    }

    return cgmap;
}

static void TestLongRunning(TPortoAPI &api, const string &name) {
    string pid;

    cerr << "Spawn long running task" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(TaskRunning(api, pid, name) == true);

    cerr << "Check that task namespaces are correct" << endl;
    Expect(GetNamespace("self", "pid") != GetNamespace(pid, "pid"));
    Expect(GetNamespace("self", "mnt") != GetNamespace(pid, "mnt"));
    Expect(GetNamespace("self", "ipc") == GetNamespace(pid, "ipc"));
    Expect(GetNamespace("self", "net") == GetNamespace(pid, "net"));
    Expect(GetNamespace("self", "user") == GetNamespace(pid, "user"));
    Expect(GetNamespace("self", "uts") == GetNamespace(pid, "uts"));

    cerr << "Check that task cgroups are correct" << endl;
    auto cgmap = GetCgroups("self");
    for (auto name : cgmap) {
        Expect(name.second == "/");
    }

    cgmap = GetCgroups(pid);
    Expect(cgmap.size() == 3);
    for (auto kv : cgmap) {
        Expect(kv.second == "/" + name);
    }

    ExpectSuccess(api.Stop(name));
    Expect(TaskRunning(api, pid, name) == false);
}

static void TestIsolation(TPortoAPI &api, const string &name) {
    string ret;
    string pid;

    cerr << "Make sure PID isolation works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);

    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("1\n"));
}

int Selftest() {
    TPortoAPI api;

    try {
        TestHolder(api);
        TestEmpty(api);

        ExpectSuccess(api.Create("a"));
        TestExitStatus(api, "a");
        TestStreams(api, "a");
        TestLongRunning(api, "a");
        TestIsolation(api, "a");
        ExpectSuccess(api.Destroy("a"));

    } catch (string e) {
        cerr << "Exception: " << e << endl;
        return 1;
    }

    return 0;
}
