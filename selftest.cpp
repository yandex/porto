#include <iostream>
#include <iomanip>
#include <sstream>

#include "rpc.pb.h"
#include "libporto.hpp"

extern "C" {
#include <unistd.h>
}

using namespace std;

#define ExpectSuccess(ret) _ExpectFailure(ret, 0, __LINE__, __func__)
#define ExpectFailure(ret, exp) _ExpectFailure(ret, exp, __LINE__, __func__)
static void _ExpectFailure(int ret, int exp, int line, const char *func) {
    if (ret != exp)
        throw string(to_string(ret) + " != " + to_string(exp) + " at " + func + ":" + to_string(line));
}

#define ShouldBeEq(A, B) _ShouldBeEq(A, B, __LINE__, __func__)
static void _ShouldBeEq(const int a, const int b, int line, const char *func) {
    if (a != b)
        throw string(to_string(a) + " != " + to_string(b) + " at " + func + ":" + to_string(line));
}
static void _ShouldBeEq(const string &a, const string &b, int line, const char *func) {
    if (a != b)
        throw string(a + " != " + b + " at " + func + ":" + to_string(line));
}

static void ShouldHaveOnlyRoot(TPortoAPI &api) {
    std::vector<std::string> containers;

    containers.clear();
    ExpectSuccess(api.List(containers));
    ShouldBeEq(containers.size(), 1);
    ShouldBeEq(containers[0], string("/"));
}

static void ShouldHaveValidProperties(TPortoAPI &api, const string &name) {
    string v;

    ExpectSuccess(api.GetProperty(name, "command", v));
    ShouldBeEq(v, string("id"));
    ExpectSuccess(api.GetProperty(name, "low_limit", v));
    ShouldBeEq(v, string("0"));
    ExpectSuccess(api.GetProperty(name, "user", v));
    ShouldBeEq(v, string("nobody"));
    ExpectSuccess(api.GetProperty(name, "group", v));
    ShouldBeEq(v, string("nogroup"));
    ExpectSuccess(api.GetProperty(name, "env", v));
    ShouldBeEq(v, string(""));
}

static void ShouldHaveValidData(TPortoAPI &api, const string &name) {
    string v;

    ExpectSuccess(api.GetData(name, "state", v));
    ShouldBeEq(v, string("stopped"));
    ExpectSuccess(api.GetData(name, "exit_status", v));
    ShouldBeEq(v, string("-1"));
    ExpectSuccess(api.GetData(name, "root_pid", v));
    ShouldBeEq(v, string("-1"));
    ExpectSuccess(api.GetData(name, "stdout", v));
    ShouldBeEq(v, string(""));
    ExpectSuccess(api.GetData(name, "stderr", v));
    ShouldBeEq(v, string(""));
    ExpectSuccess(api.GetData(name, "cpu_usage", v));
    ShouldBeEq(v, string("-1"));
    ExpectSuccess(api.GetData(name, "mem_usage", v));
    ShouldBeEq(v, string("-1"));
}

static void TestHolder(TPortoAPI &api) {
    ShouldHaveOnlyRoot(api);

    std::vector<std::string> containers;

    cerr << "Create container A" << endl;
    ExpectSuccess(api.Create("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    ShouldBeEq(containers.size(), 2);
    ShouldBeEq(containers[0], string("/"));
    ShouldBeEq(containers[1], string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    cerr << "Try to create existing container A" << endl;
    ExpectFailure(api.Create("a"), rpc::EContainerError::ContainerAlreadyExists);
    containers.clear();
    ExpectSuccess(api.List(containers));
    ShouldBeEq(containers.size(), 2);
    ShouldBeEq(containers[0], string("/"));
    ShouldBeEq(containers[1], string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    cerr << "Create container B" << endl;
    ExpectSuccess(api.Create("b"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    ShouldBeEq(containers.size(), 3);
    ShouldBeEq(containers[0], string("/"));
    ShouldBeEq(containers[1], string("a"));
    ShouldBeEq(containers[2], string("b"));
    ShouldHaveValidProperties(api, "b");
    ShouldHaveValidData(api, "b");

    cerr << "Remove container A" << endl;
    ExpectSuccess(api.Destroy("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    ShouldBeEq(containers.size(), 2);
    ShouldBeEq(containers[0], string("/"));
    ShouldBeEq(containers[1], string("b"));

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
    ShouldBeEq(ret, string("0;0;1"));

    cerr << "Check exit status of 'true'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    ShouldBeEq(ret, string("0;0;0"));

    cerr << "Check exit status of invalid command" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    // TODO: this may blow things up inside portod
    ShouldBeEq(pid, "0");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    ShouldBeEq(ret, string("2;0;0"));
}

static void TestStreams(TPortoAPI &api, const string &name) {
    string pid;
    string ret;

    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);
    usleep(1000000);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    ShouldBeEq(ret, string("out\n"));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    ShouldBeEq(ret, string(""));

    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitPid(api, pid, name);
    usleep(1000000);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    ShouldBeEq(ret, string(""));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    ShouldBeEq(ret, string("err\n"));
}

static void TestLongRunning(TPortoAPI &api, const string &name) {
    // TODO: sleep 100 and check that process is actually running
}

static void TestIsolation(TPortoAPI &api, const string &name) {
    // TODO: ps aux and check output
}

int Selftest() {
    TPortoAPI api;

    try {
        TestHolder(api);

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
