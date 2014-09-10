#include <iostream>
#include <iomanip>
#include <csignal>
#include <cstdio>

#include "rpc.pb.h"
#include "libporto.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "test.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
}

using namespace std;

namespace Test {

#define Expect(ret) ExpectReturn([&]{ return ret; }, true, 1, __LINE__, __func__)
#define ExpectSuccess(ret) ExpectReturn([&]{ return ret; }, 0, 1, __LINE__, __func__)
#define ExpectFailure(ret, exp) ExpectReturn([&]{ return ret; }, exp, 1, __LINE__, __func__)

static void ExpectCorrectCgroups(const string &pid, const string &name) {
    auto cgmap = GetCgroups(pid);
    Expect(cgmap.size() == 4);
    for (auto kv : cgmap) {
        Expect(kv.second == "/porto/" + name);
    }
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
    ExpectSuccess(api.GetProperty(name, "user", v));
    Expect(v == string("nobody"));
    ExpectSuccess(api.GetProperty(name, "group", v));
    Expect(v == string("nogroup"));
    ExpectSuccess(api.GetProperty(name, "env", v));
    Expect(v == string(""));
    ExpectSuccess(api.GetProperty(name, "memory_guarantee", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "memory_limit", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "cpu_policy", v));
    Expect(v == string("normal"));
    ExpectSuccess(api.GetProperty(name, "cpu_priority", v));
    Expect(v == string("50"));
    ExpectSuccess(api.GetProperty(name, "net_guarantee", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "net_ceil", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "net_priority", v));
    Expect(v == string("50"));
    ExpectSuccess(api.GetProperty(name, "respawn", v));
    Expect(v == string("false"));
}

static void ShouldHaveValidData(TPortoAPI &api, const string &name) {
    string v;

    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == string("stopped"));
    ExpectFailure(api.GetData(name, "exit_status", v), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", v));
    Expect(v == string("-1"));
    ExpectFailure(api.GetData(name, "root_pid", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "stdout", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "stderr", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "cpu_usage", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "memory_usage", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "net_tx", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "net_rx", v), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "parent", v));
    Expect(v == string("/"));
}

static void TestHolder(TPortoAPI &api) {
    ShouldHaveOnlyRoot(api);

    std::vector<std::string> containers;

    Say() << "Create container A" << endl;
    ExpectSuccess(api.Create("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Try to create existing container A" << endl;
    ExpectFailure(api.Create("a"), EError::ContainerAlreadyExists);
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Create container B" << endl;
    ExpectSuccess(api.Create("b"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("b"));
    ShouldHaveValidProperties(api, "b");
    ShouldHaveValidData(api, "b");

    Say() << "Remove container A" << endl;
    ExpectSuccess(api.Destroy("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("b"));

    Say() << "Remove container B" << endl;
    ExpectSuccess(api.Destroy("b"));

    Say() << "Try to execute operations on invalid container" << endl;
    ExpectFailure(api.Start("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Stop("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Pause("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Resume("a"), EError::ContainerDoesNotExist);

    string value;
    ExpectFailure(api.GetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.SetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.GetData("a", "root_pid", value), EError::ContainerDoesNotExist);

    Say() << "Try to create container with invalid name" << endl;
    string name;

    name = "z@";
    ExpectFailure(api.Create(name), EError::InvalidValue);

    name = "/invalid";
    ExpectFailure(api.Create(name), EError::InvalidValue);

    name = "invalid/";
    ExpectFailure(api.Create(name), EError::InvalidValue);

    name = "i//nvalid";
    ExpectFailure(api.Create(name), EError::InvalidValue);

    name = "invalid//";
    ExpectFailure(api.Create(name), EError::InvalidValue);

    name = "invali//d";
    ExpectFailure(api.Create(name), EError::InvalidValue);

    name = string(128, 'a');
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.Destroy(name));

    name = string(128, 'z');
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.Destroy(name));

    name = string(129, 'z');
    ExpectFailure(api.Create(name), EError::InvalidValue);

    string parent = "a";
    string child = "a/b";
    ExpectFailure(api.Create(child), EError::InvalidValue);
    ExpectSuccess(api.Create(parent));
    ExpectSuccess(api.Create(child));
    ExpectFailure(api.Destroy(parent), EError::InvalidState);
    ExpectSuccess(api.Destroy(child));
    ExpectSuccess(api.Destroy(parent));

    Say() << "Test hierarchy" << endl;
    ExpectSuccess(api.Create("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));

    ExpectSuccess(api.Create("a/b"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("a/b"));

    Say() << "Make sure child can stop only when parent is running" << endl;

    ExpectSuccess(api.Create("a/b/c"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 4);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("a/b"));
    Expect(containers[3] == string("a/b/c"));

    ExpectSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty("a/b", "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty("a/b/c", "command", "sleep 1000"));
    ExpectFailure(api.Start("a/b/c"), EError::InvalidState);
    ExpectFailure(api.Start("a/b"), EError::InvalidState);
    ExpectSuccess(api.Start("a"));
    ExpectFailure(api.Start("a/b/c"), EError::InvalidState);
    ExpectSuccess(api.Start("a/b"));
    ExpectSuccess(api.Start("a/b/c"));

    Say() << "Make sure when parent stops/dies children are stopped" << endl;

    string state;

    ExpectSuccess(api.GetData("a/b/c", "state", state));
    Expect(state == "running");
    Expect(CgExists("memory", "a") == true);
    Expect(CgExists("memory", "a/b") == true);
    Expect(CgExists("memory", "a/b/c") == true);

    ExpectSuccess(api.Stop("a/b"));
    ExpectSuccess(api.GetData("a/b/c", "state", state));
    Expect(state == "stopped");
    ExpectSuccess(api.GetData("a/b", "state", state));
    Expect(state == "stopped");
    ExpectSuccess(api.GetData("a", "state", state));
    Expect(state == "running");
    Expect(CgExists("memory", "a") == true);
    Expect(CgExists("memory", "a/b") == false);
    Expect(CgExists("memory", "a/b/c") == false);

    ExpectSuccess(api.SetProperty("a/b", "command", "sleep 1"));
    ExpectSuccess(api.Start("a/b"));
    ExpectSuccess(api.Start("a/b/c"));
    Expect(CgExists("memory", "a") == true);
    Expect(CgExists("memory", "a/b") == true);
    Expect(CgExists("memory", "a/b/c") == true);
    string pid;
    ExpectSuccess(api.GetData("a/b", "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData("a/b", "state", state));
    Expect(state == "dead");
    ExpectSuccess(api.GetData("a/b/c", "state", state));
    Expect(state == "stopped");
    Expect(CgExists("memory", "a") == true);
    Expect(CgExists("memory", "a/b") == false);
    Expect(CgExists("memory", "a/b/c") == false);

    ExpectSuccess(api.Destroy("a/b/c"));
    ExpectSuccess(api.Destroy("a/b"));
    ExpectSuccess(api.Destroy("a"));

    ShouldHaveOnlyRoot(api);
}

static void TestEmpty(TPortoAPI &api) {
    Say() << "Make sure we can't start empty container" << endl;
    ExpectSuccess(api.Create("b"));
    ExpectFailure(api.Start("b"), EError::InvalidValue);
    ExpectSuccess(api.Destroy("b"));
}

static bool TaskRunning(TPortoAPI &api, const string &pid) {
    int p = stoi(pid);
    return kill(p, 0) == 0;
}

static bool TaskZombie(TPortoAPI &api, const string &pid) {
    return GetState(pid) == "Z";
}

static void TestExitStatus(TPortoAPI &api) {
    string pid;
    string ret;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check exit status of 'false'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "false"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("256"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    Say() << "Check exit status of 'true'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("0"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    Say() << "Check exit status of invalid command" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("2"));

    Say() << "Check exit status of invalid directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/__invalid__dir__"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("-2"));

    Say() << "Check exit status when killed by signal" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "cwd", ""));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    kill(stoi(pid), SIGKILL);
    WaitState(api, name, "dead");
    Expect(TaskRunning(api, pid) == false);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("9"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestStreams(TPortoAPI &api) {
    string pid;
    string ret;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Make sure stdout works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("out\n"));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.Stop(name));

    Say() << "Make sure stderr works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string("err\n"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestLongRunning(TPortoAPI &api) {
    string pid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Spawn long running task" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(TaskRunning(api, pid) == true);

    Say() << "Check that task namespaces are correct" << endl;
    Expect(GetNamespace("self", "pid") != GetNamespace(pid, "pid"));
    Expect(GetNamespace("self", "mnt") != GetNamespace(pid, "mnt"));
    Expect(GetNamespace("self", "ipc") == GetNamespace(pid, "ipc"));
    Expect(GetNamespace("self", "net") == GetNamespace(pid, "net"));
    //Expect(GetNamespace("self", "user") == GetNamespace(pid, "user"));
    Expect(GetNamespace("self", "uts") != GetNamespace(pid, "uts"));

    Say() << "Check that task cgroups are correct" << endl;
    auto cgmap = GetCgroups("self");
    for (auto name : cgmap) {
        Expect(name.second == "/");
    }

    ExpectCorrectCgroups(pid, name);

    ExpectSuccess(api.Stop(name));
    Expect(TaskRunning(api, pid) == false);

    Say() << "Check that hierarchical task cgroups are correct" << endl;

    string child = name + "/b";
    ExpectSuccess(api.Create(child));

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    ExpectCorrectCgroups(pid, name);

    ExpectSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectSuccess(api.Start(child));
    ExpectSuccess(api.GetData(child, "root_pid", pid));
    ExpectCorrectCgroups(pid, child);

    string parent;
    ExpectSuccess(api.GetData(child, "parent", parent));
    Expect(parent == name);

    ExpectSuccess(api.Destroy(child));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestIsolation(TPortoAPI &api) {
    string ret;
    string pid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Make sure PID isolation works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);

    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("1\n"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestEnvironment(TPortoAPI &api) {
    string pid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check default environment" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    string env = GetEnv(pid);
    static const char empty_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/home/nobody\0"
        "HOME=/home/nobody\0"
        "USER=nobody\0";

    Expect(memcmp(empty_env, env.data(), sizeof(empty_env)) == 0);
    ExpectSuccess(api.Stop(name));

    Say() << "Check user-defined environment" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "env", "a=b;c=d;"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    env = GetEnv(pid);
    static const char ab_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/home/nobody\0"
        "a=b\0"
        "c=d\0"
        "HOME=/home/nobody\0"
        "USER=nobody\0";

    Expect(memcmp(ab_env, env.data(), sizeof(ab_env)) == 0);
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestUserGroup(TPortoAPI &api) {
    int uid, gid;
    string pid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check default user & group" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("nobody"));
    Expect(gid == GroupGid("nogroup"));
    ExpectSuccess(api.Stop(name));

    Say() << "Check custom user & group" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "user", "daemon"));
    ExpectSuccess(api.SetProperty(name, "group", "bin"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("daemon"));
    Expect(gid == GroupGid("bin"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestCwd(TPortoAPI &api) {
    string pid;
    string cwd;
    string portod_pid, portod_cwd;

    string name = "a";
    ExpectSuccess(api.Create(name));

    TFile portod(PID_FILE);
    (void)portod.AsString(portod_pid);
    portod_cwd = GetCwd(portod_pid);

    Say() << "Check default working directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    cwd = GetCwd(pid);

    Expect(cwd == portod_cwd);
    Expect(access(portod_cwd.c_str(), F_OK) == 0);
    ExpectSuccess(api.Stop(name));
    Expect(access(portod_cwd.c_str(), F_OK) == 0);

    Say() << "Check user defined working directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/tmp"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    Expect(access("/tmp/stdout", F_OK) == 0);
    Expect(access("/tmp/stderr", F_OK) == 0);

    cwd = GetCwd(pid);

    Expect(cwd == "/tmp");
    Expect(access("/tmp", F_OK) == 0);
    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.SetProperty(name, "cwd", ""));
    Expect(access("/tmp", F_OK) == 0);

    ExpectSuccess(api.Destroy(name));
}

/*
static void TestRootProperty(TPortoAPI &api) {
    string pid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check filesystem isolation" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "pwd"));
    ExpectSuccess(api.SetProperty(name, "cwd", ""));
    ExpectSuccess(api.SetProperty(name, "root", "/root/chroot"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    string cwd = GetCwd(pid);
    WaitExit(api, pid);

    Expect(cwd == "/root/chroot");

    string v;
    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v == string("/\n"));

    // TODO: check /proc/<PID>/root
    ExpectSuccess(api.Destroy(name));
}
*/

static void TestStateMachine(TPortoAPI &api) {
    string name = "a";
    string pid;
    string v;

    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "stopped");

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "running");

    ExpectFailure(api.Start(name), EError::InvalidState);

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "dead");

    ExpectFailure(api.Start(name), EError::InvalidState);

    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "stopped");

    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "stopped");

    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'while :; do :; done'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    v = GetState(pid);
    Expect(v == "R");

    ExpectSuccess(api.Pause(name));
    v = GetState(pid);
    Expect(v == "D");

    ExpectFailure(api.Pause(name), EError::InvalidState);

    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "paused");

    ExpectSuccess(api.Resume(name));
    v = GetState(pid);
    Expect(v == "R");

    ExpectFailure(api.Resume(name), EError::InvalidState);

    ExpectSuccess(api.Stop(name));
    Expect(TaskRunning(api, pid) == false);

    Say() << "Make sure we can stop unintentionally frozen container " << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    v = GetFreezer(name);
    Expect(v == "THAWED\n");

    SetFreezer(name, "FROZEN");

    v = GetFreezer(name);
    Expect(v == "FROZEN\n");

    ExpectSuccess(api.Stop(name));

    Say() << "Make sure we can remove paused container " << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.Pause(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestRoot(TPortoAPI &api) {
    string v;
    string root = "/";
    vector<string> properties = {
        "command",
        "user",
        "group",
        "env",
        "cwd",
        "memory_guarantee",
        "memory_limit",
        "cpu_policy",
        "cpu_priority",
        "net_guarantee",
        "net_ceil",
        "net_priority",
        "respawn",
        "cpu.smart",
        "memory.limit_in_bytes",
        "memory.low_limit_in_bytes",
        "memory.recharge_on_pgfault",
    };

    std::vector<TProperty> plist;

    ExpectSuccess(api.Plist(plist));
    Expect(plist.size() == properties.size());

    Say() << "Check root properties & data" << endl;
    for (auto p : properties)
        ExpectFailure(api.GetProperty(root, p, v), EError::InvalidProperty);

    ExpectSuccess(api.GetData(root, "state", v));
    Expect(v == string("running"));
    ExpectFailure(api.GetData(root, "exit_status", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "start_errno", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "root_pid", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "stdout", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "parent", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "stderr", v), EError::InvalidData);

    ExpectFailure(api.Stop(root), EError::InvalidState);
    ExpectFailure(api.Destroy(root), EError::InvalidValue);

    Say() << "Check root cpu_usage & memory_usage" << endl;
    ExpectSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "memory_usage", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_tx", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_rx", v));
    Expect(v == "0");

    string name = "a";
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));

    string pid;
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);

    ExpectSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v != "0");
    ExpectSuccess(api.GetData(root, "memory_usage", v));
    Expect(v != "0");

    ExpectSuccess(api.GetData(name, "cpu_usage", v));
    Expect(v != "0");
    ExpectSuccess(api.GetData(name, "memory_usage", v));
    Expect(v != "0");

    // TODO: net_tx, net_rx

    ExpectSuccess(api.Destroy(name));
}

static bool CanTestLimits() {
    if (!HaveCgKnob("memory", "", "memory.low_limit_in_bytes"))
        return false;

    if (!HaveCgKnob("cpu", "", "cpu.smart"))
        return false;

    return true;
}

static void TestLimits(TPortoAPI &api) {
    string pid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check default limits" << endl;
    string current;

    current = GetCgKnob("memory", "", "memory.use_hierarchy");
    Expect(current == "1");

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.use_hierarchy");
    Expect(current == "1");

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == to_string(LLONG_MAX) || current == to_string(ULLONG_MAX));

    if (HaveCgKnob("memory", name, "memory.low_limit_in_bytes")) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        Expect(current == "0");
    }
    ExpectSuccess(api.Stop(name));

    Say() << "Check custom limits" << endl;
    string exp_limit = "524288";
    string exp_guar = "16384";
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    if (HaveCgKnob("memory", name, "memory.low_limit_in_bytes"))
        ExpectSuccess(api.SetProperty(name, "memory_guarantee", exp_guar));
    ExpectSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == exp_limit);
    ExpectSuccess(api.Stop(name));
    if (HaveCgKnob("memory", name, "memory.low_limit_in_bytes")) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        Expect(current == exp_guar);
    }

    Say() << "Check cpu_priority" << endl;
    ExpectFailure(api.SetProperty(name, "cpu_priority", "-1"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "cpu_priority", "100"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "cpu_priority", "0"));
    ExpectSuccess(api.SetProperty(name, "cpu_priority", "99"));

    Say() << "Check cpu_policy" << endl;
    string smart;

    ExpectFailure(api.SetProperty(name, "cpu_policy", "somecrap"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "cpu_policy", "idle"), EError::NotSupported);

    if (HaveCgKnob("cpu", name, "cpu.smart")) {
        ExpectSuccess(api.SetProperty(name, "cpu_policy", "rt"));
        ExpectSuccess(api.Start(name));
        smart = GetCgKnob("cpu", name, "cpu.smart");
        Expect(smart == "1");
        ExpectSuccess(api.Stop(name));

        ExpectSuccess(api.SetProperty(name, "cpu_policy", "normal"));
        ExpectSuccess(api.Start(name));
        smart = GetCgKnob("cpu", name, "cpu.smart");
        Expect(smart == "0");
        ExpectSuccess(api.Stop(name));
    }

    string shares;
    ExpectSuccess(api.SetProperty(name, "cpu_policy", "normal"));
    ExpectSuccess(api.SetProperty(name, "cpu_priority", "0"));
    ExpectSuccess(api.Start(name));
    shares = GetCgKnob("cpu", name, "cpu.shares");
    Expect(shares == "2");
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "cpu_priority", "50"));
    ExpectSuccess(api.Start(name));
    shares = GetCgKnob("cpu", name, "cpu.shares");
    Expect(shares == "52");
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "cpu_priority", "99"));
    ExpectSuccess(api.Start(name));
    shares = GetCgKnob("cpu", name, "cpu.shares");
    Expect(shares == "101");
    ExpectSuccess(api.Stop(name));

    // TODO: net_guarantee, net_ceil, net_priority

    ExpectSuccess(api.Destroy(name));
}

static void TestDynamic(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    string current;
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == to_string(LLONG_MAX) || current == to_string(ULLONG_MAX));

    string exp_limit = "268435456";
    ExpectSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == exp_limit);

    ExpectSuccess(api.Pause(name));

    exp_limit = "536870912";
    ExpectSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == exp_limit);

    ExpectSuccess(api.Resume(name));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestLimitsHierarchy(TPortoAPI &api) {
    string pid;

    if (!HaveCgKnob("memory", "", "memory.low_limit_in_bytes"))
        return;

    //
    // box +-- monitoring
    //     |
    //     +-- system
    //     |
    //     +-- production +-- slot1
    //                    |
    //                    +-- slot2
    //

    string box = "box";
    string prod = "box/production";
    string slot1 = "box/production/slot1";
    string slot2 = "box/production/slot2";
    string system = "box/system";
    string monit = "box/monitoring";

    ExpectSuccess(api.Create(box));
    ExpectSuccess(api.Create(prod));
    ExpectSuccess(api.Create(slot1));
    ExpectSuccess(api.Create(slot2));
    ExpectSuccess(api.Create(system));
    ExpectSuccess(api.Create(monit));

    size_t total = GetTotalMemory();

    Say() << "Single container can't go over reserve" << endl;
    ExpectFailure(api.SetProperty(system, "memory_guarantee", to_string(total)), EError::ResourceNotAvailable);
    ExpectSuccess(api.SetProperty(system, "memory_guarantee", to_string(total - MEMORY_GUARANTEE_RESERVE)));

    Say() << "Distributed guarantee can't go over reserve" << endl;
    size_t chunk = (total - MEMORY_GUARANTEE_RESERVE) / 4;

    ExpectSuccess(api.SetProperty(system, "memory_guarantee", to_string(chunk)));
    ExpectSuccess(api.SetProperty(monit, "memory_guarantee", to_string(chunk)));
    ExpectSuccess(api.SetProperty(slot1, "memory_guarantee", to_string(chunk)));
    ExpectFailure(api.SetProperty(slot2, "memory_guarantee", to_string(chunk + 1)), EError::ResourceNotAvailable);
    ExpectSuccess(api.SetProperty(slot2, "memory_guarantee", to_string(chunk)));

    ExpectSuccess(api.SetProperty(monit, "memory_guarantee", to_string(0)));
    ExpectSuccess(api.SetProperty(system, "memory_guarantee", to_string(0)));

    auto CheckPropertyHierarhcy = [&](TPortoAPI &api, const std::string &property) {
        Say() << "Parent can't have less guarantee than sum of children" << endl;
        ExpectSuccess(api.SetProperty(slot1, property, to_string(chunk)));
        ExpectSuccess(api.SetProperty(slot2, property, to_string(chunk)));
        ExpectFailure(api.SetProperty(prod, property, to_string(chunk)), EError::InvalidValue);
        ExpectFailure(api.SetProperty(box, property, to_string(chunk)), EError::InvalidValue);

        Say() << "Child can't go over parent guarantee" << endl;
        ExpectSuccess(api.SetProperty(prod, property, to_string(2 * chunk)));
        ExpectFailure(api.SetProperty(slot1, property, to_string(2 * chunk)), EError::InvalidValue);

        Say() << "Can lower guarantee if possible" << endl;
        ExpectFailure(api.SetProperty(prod, property, to_string(chunk)), EError::InvalidValue);
        ExpectSuccess(api.SetProperty(slot2, property, to_string(0)));
        ExpectSuccess(api.SetProperty(prod, property, to_string(chunk)));
    };

    CheckPropertyHierarhcy(api, "memory_guarantee");
    CheckPropertyHierarhcy(api, "memory_limit");

    ExpectSuccess(api.Destroy(monit));
    ExpectSuccess(api.Destroy(system));
    ExpectSuccess(api.Destroy(slot2));
    ExpectSuccess(api.Destroy(slot1));
    ExpectSuccess(api.Destroy(prod));
    ExpectSuccess(api.Destroy(box));
}

static void TestPermissions(TPortoAPI &api) {
    struct stat st;
    string path;

    string name = "a";
    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    path = "/sys/fs/cgroup/memory/porto";
    Expect(lstat(path.c_str(), &st) == 0);
    Expect(st.st_mode == (0755 | S_IFDIR));

    path = "/sys/fs/cgroup/memory/porto/" + name;
    Expect(lstat(path.c_str(), &st) == 0);
    Expect(st.st_mode == (0755 | S_IFDIR));

    path = "/sys/fs/cgroup/memory/porto/" + name + "/tasks";
    Expect(lstat(path.c_str(), &st) == 0);
    Expect(st.st_mode == (0644 | S_IFREG));

    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestRespawn(TPortoAPI &api) {
    string pid, respawnPid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1"));
    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.Start(name));

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "root_pid", respawnPid));
    Expect(pid != respawnPid);

    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.SetProperty(name, "respawn", "false"));

    ExpectSuccess(api.Destroy(name));
}

static void TestLeaks(TPortoAPI &api) {
    string pid;
    string name;
    int nr = 1000;
    int slack = 4096;

    TFile f(PID_FILE);
    Expect(f.AsString(pid) == false);

    for (int i = 0; i < nr; i++) {
        name = "a" + to_string(i);
        ExpectSuccess(api.Create(name));
        ExpectSuccess(api.SetProperty(name, "command", "true"));
        ExpectSuccess(api.Start(name));
    }

    for (int i = 0; i < nr; i++) {
        name = "a" + to_string(i);
        ExpectSuccess(api.Destroy(name));
    }

    int prev = GetVmRss(pid);

    for (int i = 0; i < nr; i++) {
        name = "b" + to_string(i);
        ExpectSuccess(api.Create(name));
        ExpectSuccess(api.SetProperty(name, "command", "true"));
        ExpectSuccess(api.Start(name));
    }

    for (int i = 0; i < nr; i++) {
        name = "b" + to_string(i);
        ExpectSuccess(api.Destroy(name));
    }

    int now = GetVmRss(pid);

    Expect(now <= prev + slack);
}

static void TestRecovery(TPortoAPI &api) {
    string pid, v;
    string name = "a";

    map<string,string> props = {
        { "command", "sleep 1000" },
        { "user", "bin" },
        { "group", "daemon" },
        { "env", "a=a;b=b" },
    };

    Say() << "Make sure we don't kill containers when doing recovery" << endl;
    ExpectSuccess(api.Create(name));

    for (auto &pair : props)
        ExpectSuccess(api.SetProperty(name, pair.first, pair.second));
    ExpectSuccess(api.Start(name));

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(TaskRunning(api, pid) == true);
    Expect(TaskZombie(api, pid) == false);

    string portod_pid;
    TFile portod(PID_FILE);
    ExpectSuccess(portod.AsString(portod_pid));

    kill(stoi(portod_pid), SIGKILL);
    WaitExit(api, portod_pid);
    WaitPortod(api);

    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "running");
    ExpectSuccess(api.GetData(name, "root_pid", v));
    Expect(v == pid);

    Expect(TaskRunning(api, pid) == true);
    Expect(TaskZombie(api, pid) == false);

    for (auto &pair : props) {
        string v;
        ExpectSuccess(api.GetProperty(name, pair.first, v));
        Expect(v == pair.second);
    }

    ExpectSuccess(api.Destroy(name));

    Say() << "Make sure hierarchical recovery works" << endl;

    string parent = "a";
    string child = "a/b";
    ExpectSuccess(api.Create(parent));
    ExpectSuccess(api.Create(child));

    ExpectSuccess(portod.AsString(portod_pid));

    kill(stoi(portod_pid), SIGKILL);
    WaitExit(api, portod_pid);
    WaitPortod(api);

    std::vector<std::string> containers;

    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("a/b"));
    ExpectSuccess(api.Destroy(child));
    ExpectSuccess(api.Destroy(parent));
}

int SelfTest(string name) {
    pair<string, function<void(TPortoAPI &)>> tests[] = {
        { "root", TestRoot },
        { "holder", TestHolder },
        { "empty", TestEmpty },
        { "state_machine", TestStateMachine },
        { "exit_status", TestExitStatus },
        { "streams", TestStreams },
        { "long_running", TestLongRunning },
        { "isolation", TestIsolation },
        { "environment", TestEnvironment },
        { "user_group", TestUserGroup },
        { "cwd", TestCwd },
        //{ "root", TestRootProperty },
        { "limits", TestLimits },
        { "dynamic", TestDynamic },
        { "permissions", TestPermissions },
        { "respawn", TestRespawn },
        { "hierarchy", TestLimitsHierarchy },
        { "leaks", TestLeaks },

        { "daemon", TestDaemon },
        { "recovery", TestRecovery },
    };

    int respawns = 0;
    try {
        TPortoAPI api;

        cerr << ">>> Truncating logs and restarting porto..." << endl;

        (void)truncate(LOG_FILE.c_str(), 0);
        (void)truncate(LOOP_LOG_FILE.c_str(), 0);

        int pid;
        if (StringToInt(Pgrep("portoloop"), pid))
            throw string("Can't find portoloop pid");

        if (kill(pid, SIGHUP))
            throw string("Can't send SIGHUP to portoloop");

        WaitPortod(api);

        for (auto t : tests) {
            if (name.length() && name != t.first)
                continue;

            cerr << ">>> Testing " << t.first << "..." << endl;
            t.second(api);
        }

        vector<string> lines;
        TFile log(LOOP_LOG_FILE);
        if (log.AsLines(lines))
            throw string("Can't read portoloop log");

        for (auto s : lines) {
            if (s.find("Spawned") != string::npos)
                respawns++;
        }
    } catch (string e) {
        cerr << "EXCEPTION: " << e << endl;
        return 1;
    }

    cerr << "SUCCESS: All tests successfully passed!" << endl;
    if (!CanTestLimits())
        cerr << "WARNING: Due to missing kernel support, memory_guarantee/cpu_policy has not been tested!" << endl;
    if (respawns != 1 + 2)
        cerr << "WARNING: Unexpected number of portoloop respawns: " << respawns << "!" << endl;

    return 0;
}
}
