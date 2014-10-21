#include <iostream>
#include <iomanip>
#include <csignal>
#include <cstdio>
#include <algorithm>

#include "rpc.pb.h"
#include "libporto.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/pwd.hpp"
#include "test.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
}

using std::string;
using std::vector;
using std::map;
using std::pair;

namespace test {

static vector<string> subsystems = { "net_cls", "freezer", "memory", "cpu", "cpuacct" };
static vector<string> namespaces = { "pid", "mnt", "ipc", "net", /*"user", */"uts" };

static void ExpectCorrectCgroups(const string &pid, const string &name) {
    auto cgmap = GetCgroups(pid);
    int expected = subsystems.size();

    for (auto kv : cgmap) {
        vector<string> cgsubsystems;
        ExpectSuccess(SplitString(kv.first, ',', cgsubsystems));

        for (auto &subsys : subsystems) {
            if (std::find(cgsubsystems.begin(), cgsubsystems.end(), subsys) != cgsubsystems.end()) {
                Expect(kv.second == "/porto/" + name);
                expected--;
            }
        }
    }
    Expect(expected == 0);
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
    ExpectSuccess(api.GetProperty(name, "cwd", v));
    Expect(v == config().container().tmp_dir() + "/" + name);
    ExpectSuccess(api.GetProperty(name, "root", v));
    Expect(v == "/");
    ExpectSuccess(api.GetProperty(name, "user", v));
    Expect(v == GetDefaultUser());
    ExpectSuccess(api.GetProperty(name, "group", v));
    Expect(v == GetDefaultGroup());
    ExpectSuccess(api.GetProperty(name, "env", v));
    Expect(v == string(""));
    ExpectSuccess(api.GetProperty(name, "memory_guarantee", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "memory_limit", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "cpu_policy", v));
    Expect(v == string("normal"));
    ExpectSuccess(api.GetProperty(name, "cpu_priority", v));
    Expect(v == std::to_string(DEF_CLASS_PRIO));
    ExpectSuccess(api.GetProperty(name, "net_guarantee", v));
    Expect(v == std::to_string(DEF_CLASS_RATE));
    ExpectSuccess(api.GetProperty(name, "net_ceil", v));
    Expect(v == std::to_string(DEF_CLASS_CEIL));
    ExpectSuccess(api.GetProperty(name, "net_priority", v));
    Expect(v == std::to_string(DEF_CLASS_NET_PRIO));
    ExpectSuccess(api.GetProperty(name, "respawn", v));
    Expect(v == string("false"));
    ExpectSuccess(api.GetProperty(name, "cpu.smart", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "memory.limit_in_bytes", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "memory.low_limit_in_bytes", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "memory.recharge_on_pgfault", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetProperty(name, "stdin_path", v));
    Expect(v == string("/dev/null"));
    ExpectSuccess(api.GetProperty(name, "stdout_path", v));
    Expect(v == config().container().tmp_dir() + "/" + name + "/stdout");
    ExpectSuccess(api.GetProperty(name, "stderr_path", v));
    Expect(v == config().container().tmp_dir() + "/" + name + "/stderr");
    ExpectSuccess(api.GetProperty(name, "ulimit", v));
    Expect(v == "");
    ExpectSuccess(api.GetProperty(name, "hostname", v));
    Expect(v == "");
    ExpectSuccess(api.GetProperty(name, "bind_dns", v));
    Expect(v == "false");
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
    ExpectFailure(api.GetData(name, "net_bytes", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "net_packets", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "net_drops", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "net_overlimits", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "oom_killed", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "respawn_count", v), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "parent", v));
    ExpectFailure(api.GetData(name, "io_read", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "io_write", v), EError::InvalidState);
    Expect(v == string("/"));
}

static void ExpectTclass(string name, bool exp) {
    string cls = GetCgKnob("net_cls", name, "net_cls.classid");
    Expect(TcClassExist(cls) == exp);
}

static void TestHolder(TPortoAPI &api) {
    ShouldHaveOnlyRoot(api);

    std::vector<std::string> containers;

    Say() << "Create container A" << std::endl;
    ExpectSuccess(api.Create("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Try to create existing container A" << std::endl;
    ExpectFailure(api.Create("a"), EError::ContainerAlreadyExists);
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Create container B" << std::endl;
    ExpectSuccess(api.Create("b"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("b"));
    ShouldHaveValidProperties(api, "b");
    ShouldHaveValidData(api, "b");

    Say() << "Remove container A" << std::endl;
    ExpectSuccess(api.Destroy("a"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 2);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("b"));

    Say() << "Remove container B" << std::endl;
    ExpectSuccess(api.Destroy("b"));

    Say() << "Try to execute operations on invalid container" << std::endl;
    ExpectFailure(api.Start("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Stop("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Pause("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Resume("a"), EError::ContainerDoesNotExist);

    string value;
    ExpectFailure(api.GetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.SetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.GetData("a", "root_pid", value), EError::ContainerDoesNotExist);

    Say() << "Try to create container with invalid name" << std::endl;
    string name;

    name = "z$";
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

    Say() << "Test hierarchy" << std::endl;
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

    Say() << "Make sure child can stop only when parent is running" << std::endl;

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

    Say() << "Make sure when parent stops/dies children are stopped" << std::endl;

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

    ExpectTclass("a", true);
    ExpectTclass("a/b", true);
    ExpectTclass("a/b/c", true);

    WaitState(api, "a/b", "dead");
    ExpectSuccess(api.GetData("a/b", "state", state));
    Expect(state == "dead");
    ExpectSuccess(api.GetData("a/b/c", "state", state));
    Expect(state == "stopped");
    Expect(CgExists("memory", "a") == true);
    Expect(CgExists("memory", "a/b") == true);
    Expect(CgExists("memory", "a/b/c") == false);

    ExpectSuccess(api.Destroy("a/b/c"));
    ExpectSuccess(api.Destroy("a/b"));
    ExpectSuccess(api.Destroy("a"));

    ShouldHaveOnlyRoot(api);
}

static void TestEmpty(TPortoAPI &api) {
    Say() << "Make sure we can't start empty container" << std::endl;
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

    Say() << "Check exit status of 'false'" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "false"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("256"));
    ExpectSuccess(api.GetData(name, "oom_killed", ret));
    Expect(ret == string("false"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    Say() << "Check exit status of 'true'" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("0"));
    ExpectSuccess(api.GetData(name, "oom_killed", ret));
    Expect(ret == string("false"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    Say() << "Check exit status of invalid command" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "oom_killed", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("2"));

    Say() << "Check exit status of invalid directory" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/__invalid__dir__"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "oom_killed", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("2"));

    Say() << "Check exit status when killed by signal" << std::endl;
    ExpectSuccess(api.Destroy(name));
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    kill(stoi(pid), SIGKILL);
    WaitState(api, name, "dead");
    Expect(TaskRunning(api, pid) == false);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("9"));
    ExpectSuccess(api.GetData(name, "oom_killed", ret));
    Expect(ret == string("false"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    Say() << "Check oom_killed property" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "memory_limit", "10"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("9"));
    ExpectSuccess(api.GetData(name, "oom_killed", ret));
    Expect(ret == string("true"));

    ExpectSuccess(api.Destroy(name));
}

static void TestStreams(TPortoAPI &api) {
    string ret;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Make sure stdout works" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("out\n"));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.Stop(name));

    Say() << "Make sure stderr works" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
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

    Say() << "Spawn long running task" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(TaskRunning(api, pid) == true);

    AsRoot(api);
    Say() << "Check that task namespaces are correct" << std::endl;
    Expect(GetNamespace("self", "pid") != GetNamespace(pid, "pid"));
    Expect(GetNamespace("self", "mnt") != GetNamespace(pid, "mnt"));
    Expect(GetNamespace("self", "ipc") == GetNamespace(pid, "ipc"));
    Expect(GetNamespace("self", "net") == GetNamespace(pid, "net"));
    //Expect(GetNamespace("self", "user") == GetNamespace(pid, "user"));
    Expect(GetNamespace("self", "uts") == GetNamespace(pid, "uts"));

    Say() << "Check that task cgroups are correct" << std::endl;
    auto cgmap = GetCgroups("self");
    for (auto name : cgmap) {
        // skip systemd cgroups
        if (name.first.find("systemd") != string::npos)
            continue;

        Expect(name.second == "/");
    }

    ExpectCorrectCgroups(pid, name);
    AsNobody(api);

    string root_cls = GetCgKnob("net_cls", "/", "net_cls.classid");
    string leaf_cls = GetCgKnob("net_cls", name, "net_cls.classid");

    Expect(root_cls != "0");
    Expect(leaf_cls != "0");
    Expect(root_cls != leaf_cls);

    Expect(TcClassExist(root_cls) == true);
    Expect(TcClassExist(leaf_cls) == true);

    ExpectSuccess(api.Stop(name));
    Expect(TaskRunning(api, pid) == false);
    Expect(TcClassExist(leaf_cls) == false);

    Say() << "Check that destroying container removes tclass" << std::endl;
    ExpectSuccess(api.Start(name));
    Expect(TcClassExist(root_cls) == true);
    Expect(TcClassExist(leaf_cls) == true);
    ExpectSuccess(api.Destroy(name));
    Expect(TaskRunning(api, pid) == false);
    Expect(TcClassExist(leaf_cls) == false);
    ExpectSuccess(api.Create(name));

    Say() << "Check that hierarchical task cgroups are correct" << std::endl;

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

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Make sure PID isolation works" << std::endl;
    ExpectSuccess(api.SetProperty(name, "isolate", "false"));

    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret != string("1\n"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "command", "ps aux"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(std::count(ret.begin(), ret.end(), '\n') != 2);
    ExpectSuccess(api.Stop(name));


    ExpectSuccess(api.SetProperty(name, "isolate", "true"));
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("1\n"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "command", "ps aux"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(std::count(ret.begin(), ret.end(), '\n') == 2);

    Say() << "Make sure container has correct network class" << std::endl;

    TNetlink nl;
    Expect(nl.Open(config().network().device()) == TError::Success());
    string handle = GetCgKnob("net_cls", name, "net_cls.classid");
    Expect(handle != "0");
    Expect(nl.ClassExists(stoul(handle)) == true);
    ExpectSuccess(api.Stop(name));
    Expect(nl.ClassExists(stoul(handle)) == false);

    ExpectSuccess(api.Destroy(name));
}

static void TestProperty(TPortoAPI &api) {
    string val;
    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check property trimming" << std::endl;
    ExpectSuccess(api.SetProperty(name, "env", ""));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "");

    ExpectSuccess(api.SetProperty(name, "env", " "));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "");

    ExpectSuccess(api.SetProperty(name, "env", "    "));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "");

    ExpectSuccess(api.SetProperty(name, "env", " a"));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "a");

    ExpectSuccess(api.SetProperty(name, "env", "b "));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "b");

    ExpectSuccess(api.SetProperty(name, "env", " c "));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "c");

    ExpectSuccess(api.SetProperty(name, "env", "     d     "));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "d");

    ExpectSuccess(api.SetProperty(name, "env", "    e"));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "e");

    ExpectSuccess(api.SetProperty(name, "env", "f    "));
    ExpectSuccess(api.GetProperty(name, "env", val));
    Expect(val == "f");

    string longProperty = string(10 * 1024, 'x');
    ExpectSuccess(api.SetProperty(name, "env", longProperty));
    ExpectSuccess(api.GetProperty(name, "env", val));

    ExpectSuccess(api.Destroy(name));
}

static void ExpectEnv(TPortoAPI &api,
                      const std::string &name,
                      const std::string &env,
                      const char expected[],
                      size_t expectedLen) {
    string pid;

    ExpectSuccess(api.SetProperty(name, "env", env));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    string ret = GetEnv(pid);

    Expect(memcmp(expected, ret.data(), expectedLen) == 0);
    ExpectSuccess(api.Stop(name));
}

static void TestEnvironment(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));

    AsRoot(api);

    Say() << "Check default environment" << std::endl;

    static const char empty_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "HOME=/place/porto/a\0"
        "USER=nobody\0";
    ExpectEnv(api, name, "", empty_env, sizeof(empty_env));

    Say() << "Check user-defined environment" << std::endl;
    static const char ab_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "a=b\0"
        "c=d\0"
        "HOME=/place/porto/a\0"
        "USER=nobody\0";

    ExpectEnv(api, name, "a=b;c=d;", ab_env, sizeof(ab_env));
    ExpectEnv(api, name, "a=b;;c=d;", ab_env, sizeof(ab_env));

    static const char asb_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "a=e;b\0"
        "c=d\0"
        "HOME=/place/porto/a\0"
        "USER=nobody\0";
    ExpectEnv(api, name, "a=e\\;b;c=d;", asb_env, sizeof(asb_env));

    ExpectSuccess(api.SetProperty(name, "command", "sleep $N"));
    ExpectSuccess(api.SetProperty(name, "env", "N=1"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestUserGroup(TPortoAPI &api) {
    int uid, gid;
    string pid;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check default user & group" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid(GetDefaultUser()));
    Expect(gid == GroupGid(GetDefaultGroup()));
    ExpectSuccess(api.Stop(name));

    Say() << "Check custom user & group" << std::endl;

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));

    ExpectFailure(api.SetProperty(name, "user", "daemon"), EError::Permission);
    ExpectFailure(api.SetProperty(name, "group", "bin"), EError::Permission);

    string user, group;
    ExpectSuccess(api.GetProperty(name, "user", user));
    ExpectSuccess(api.GetProperty(name, "group", group));
    ExpectSuccess(api.SetProperty(name, "user", user));
    ExpectSuccess(api.SetProperty(name, "group", group));

    AsRoot(api);
    ExpectSuccess(api.SetProperty(name, "user", "daemon"));
    ExpectSuccess(api.SetProperty(name, "group", "bin"));
    AsNobody(api);

    /* TODO
    ExpectFailure(api.Start(name), EError::Permission);

    AsRoot(api);
    */
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("daemon"));
    Expect(gid == GroupGid("bin"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
    AsNobody(api);
}

static void TestCwd(TPortoAPI &api) {
    string pid;
    string cwd;
    string portodPid, portodCwd;

    AsRoot(api);

    string name = "a";
    ExpectSuccess(api.Create(name));

    TFile portod(config().slave_pid().path());
    (void)portod.AsString(portodPid);
    portodCwd = GetCwd(portodPid);

    Say() << "Check default working directory" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    cwd = GetCwd(pid);

    string prefix = config().container().tmp_dir();

    Expect(cwd != portodCwd);
    Expect(cwd == prefix + "/" + name);

    Expect(access(cwd.c_str(), F_OK) == 0);
    ExpectSuccess(api.Stop(name));
    Expect(access(cwd.c_str(), F_OK) != 0);
    ExpectSuccess(api.Destroy(name));

    ExpectSuccess(api.Create("b"));
    ExpectSuccess(api.SetProperty("b", "command", "sleep 1000"));
    ExpectSuccess(api.Start("b"));
    ExpectSuccess(api.GetData("b", "root_pid", pid));
    string bcwd = GetCwd(pid);
    ExpectSuccess(api.Destroy("b"));

    Expect(bcwd != portodCwd);
    Expect(bcwd == prefix + "/b");
    Expect(bcwd != cwd);

    Say() << "Check user defined working directory" << std::endl;
    ExpectSuccess(api.Create(name));
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
    Expect(access("/tmp", F_OK) == 0);

    ExpectSuccess(api.Destroy(name));

    AsNobody(api);
}

static void TestStd(TPortoAPI &api) {
    string pid;
    string name = "a";

    AsRoot(api);

    ExpectSuccess(api.Create(name));

    Say() << "Check default stdin/stdout/stderr" << std::endl;
    Expect(!FileExists("/tmp/stdout"));
    Expect(!FileExists("/tmp/stderr"));
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/tmp"));
    ExpectSuccess(api.Start(name));
    Expect(FileExists("/tmp/stdout"));
    Expect(FileExists("/tmp/stderr"));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(ReadLink("/proc/" + pid + "/fd/0") == "/dev/null");
    Expect(ReadLink("/proc/" + pid + "/fd/1") == "/tmp/stdout");
    Expect(ReadLink("/proc/" + pid + "/fd/2") == "/tmp/stderr");
    ExpectSuccess(api.Stop(name));
    Expect(!FileExists("/tmp/stdout"));
    Expect(!FileExists("/tmp/stderr"));


    Say() << "Check custom stdin/stdout/stderr" << std::endl;
    TFile f("/tmp/a_stdin");
    TError error = f.WriteStringNoAppend("hi");
    if (error)
        throw error.GetMsg();

    Expect(!FileExists("/tmp/a_stdout"));
    Expect(!FileExists("/tmp/a_stderr"));
    ExpectSuccess(api.SetProperty(name, "stdin_path", "/tmp/a_stdin"));
    ExpectSuccess(api.SetProperty(name, "stdout_path", "/tmp/a_stdout"));
    ExpectSuccess(api.SetProperty(name, "stderr_path", "/tmp/a_stderr"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(FileExists("/tmp/a_stdout"));
    Expect(FileExists("/tmp/a_stderr"));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(ReadLink("/proc/" + pid + "/fd/0") == "/tmp/a_stdin");
    Expect(ReadLink("/proc/" + pid + "/fd/1") == "/tmp/a_stdout");
    Expect(ReadLink("/proc/" + pid + "/fd/2") == "/tmp/a_stderr");
    ExpectSuccess(api.Stop(name));
    Expect(FileExists("/tmp/a_stdin"));
    Expect(!FileExists("/tmp/a_stdout"));
    Expect(!FileExists("/tmp/a_stderr"));


    Say() << "Make sure custom stdin is not removed" << std::endl;
    string ret;
    ExpectSuccess(api.SetProperty(name, "command", "cat"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("hi"));

    ExpectSuccess(api.Destroy(name));

    AsNobody(api);
}

static std::string StartWaitAndGetData(TPortoAPI &api, const std::string &name, const std::string &data) {
    string v;
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, data, v));
    return v;
}

static map<string, string> ParseMountinfo(string s) {
    map<string, string> m;
    vector<string> lines;

    TError error = SplitString(s, '\n', lines);
    if (error)
        throw error.GetMsg();

    for (auto &line : lines) {
        vector<string> tok;
        TError error = SplitString(line, ' ', tok);
        if (error)
            throw error.GetMsg();

        m[tok[4]] = tok[5];
    }

    return m;
}

static void TestRootProperty(TPortoAPI &api) {
    string pid;

    string name = "a";
    string path = config().container().tmp_dir() + "/" + name;
    ExpectSuccess(api.Create(name));

    Say() << "Check filesystem isolation" << std::endl;

    AsRoot(api);
    BootstrapCommand("/bin/sleep", path);
    BootstrapCommand("/bin/pwd", path, false);
    BootstrapCommand("/bin/ls", path, false);
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    string bindDns;

    ExpectSuccess(api.GetProperty(name, "bind_dns", bindDns));
    Expect(bindDns == "false");

    ExpectSuccess(api.SetProperty(name, "root", path));

    string cwd;
    ExpectSuccess(api.GetProperty(name, "cwd", cwd));
    Expect(cwd == "/");

    ExpectSuccess(api.GetProperty(name, "bind_dns", bindDns));
    Expect(bindDns == "true");

    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    AsRoot(api);
    cwd = GetCwd(pid);
    string root = GetRoot(pid);
    AsNobody(api);
    ExpectSuccess(api.Stop(name));

    Expect(cwd == path);
    Expect(root == path);

    ExpectSuccess(api.SetProperty(name, "command", "/pwd"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");

    string v;
    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v == string("/\n"));
    ExpectSuccess(api.Stop(name));

    Say() << "Check /dev layout" << std::endl;

    ExpectSuccess(api.SetProperty(name, "command", "/ls -1 /dev"));
    v = StartWaitAndGetData(api, name, "stdout");

    vector<string> devs = { "null", "zero", "full", "tty", "urandom", "random" };
    vector<string> other = { "ptmx", "pts", "shm" };
    vector<string> tokens;
    TError error = SplitString(v, '\n', tokens);
    if (error)
        throw error.GetMsg();

    Expect(devs.size() + other.size() == tokens.size());
    for (auto &dev : devs)
        Expect(std::find(tokens.begin(), tokens.end(), dev) != tokens.end());

    ExpectSuccess(api.Stop(name));

    Say() << "Check /proc restrictions" << std::endl;

    AsRoot(api);
    BootstrapCommand("/bin/cat", path);
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    v = StartWaitAndGetData(api, name, "stdout");

    auto m = ParseMountinfo(v);
    Expect(m["/etc/resolv.conf"] == "ro,relatime");
    Expect(m["/etc/hosts"] == "ro,relatime");
    Expect(m["/sys"] == "ro,nosuid,nodev,noexec,relatime");
    Expect(m["/proc/sys"] == "ro,relatime");
    Expect(m["/proc/sysrq-trigger"] == "ro,relatime");
    Expect(m["/proc/irq"] == "ro,relatime");
    Expect(m["/proc/bus"] == "ro,relatime");

    ExpectSuccess(api.Stop(name));

    Say() << "Make sure /dev /sys /proc are not mounted when root is not isolated " << std::endl;

    TFolder f(path);
    AsRoot(api);
    error = f.Remove(true);
    if (error)
        throw error.GetMsg();
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "root", "/"));
    ExpectSuccess(api.SetProperty(name, "command", "ls -1 " + cwd));

    v = StartWaitAndGetData(api, name, "stdout");
    Expect(v == "stderr\nstdout\n");

    ExpectSuccess(api.Destroy(name));
}

static string GetHostname() {
    char buf[1024];
    Expect(gethostname(buf, sizeof(buf)) == 0);
    return buf;
}

static void TestHostnameProperty(TPortoAPI &api) {
    string pid, v;
    string name = "a";
    string host = "porto_" + name;
    string path = config().container().tmp_dir() + "/" + name;
    ExpectSuccess(api.Create(name));

    AsRoot(api);
    BootstrapCommand("/bin/hostname", path);
    BootstrapCommand("/bin/sleep", path, false);
    AsNobody(api);
    ExpectSuccess(api.SetProperty(name, "root", path));

    Say() << "Check default hostname" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    AsRoot(api);
    Expect(GetNamespace("self", "uts") == GetNamespace(pid, "uts"));
    AsNobody(api);
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "command", "/hostname"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v == GetHostname() + "\n");
    ExpectSuccess(api.Stop(name));

    Say() << "Check custom hostname" << std::endl;
    ExpectSuccess(api.SetProperty(name, "hostname", host));

    ExpectSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    AsRoot(api);
    Expect(GetNamespace("self", "uts") != GetNamespace(pid, "uts"));
    AsNobody(api);
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "command", "/hostname"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v != GetHostname() + "\n");
    Expect(v == host + "\n");
    ExpectSuccess(api.Stop(name));

    Say() << "Check /etc/hostname" << std::endl;
    TFolder d(path + "/etc");
    TFile f(path + "/etc/hostname");
    AsRoot(api);
    if (!d.Exists())
        ExpectSuccess(d.Create());
    ExpectSuccess(f.Touch());
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "/hostname"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    AsRoot(api);
    ExpectSuccess(f.AsString(v));
    AsNobody(api);
    Expect(v == host + "\n");
    AsRoot(api);
    ExpectSuccess(d.Remove(true));
    AsNobody(api);

    ExpectSuccess(api.Destroy(name));
}

static void TestBindProperty(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check bind parsing" << std::endl;
    ExpectFailure(api.SetProperty(name, "bind", "/tmp"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "bind", "qwerty /tmp"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "bind", "/tmp /bin"));
    ExpectFailure(api.SetProperty(name, "bind", "/tmp /bin xyz"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "bind", "/tmp /bin ro"));
    ExpectSuccess(api.SetProperty(name, "bind", "/tmp /bin rw"));
    ExpectFailure(api.SetProperty(name, "bind", "/tmp /bin ro; q"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "bind", "/tmp /bin ro; /bin /sbin"));

    Say() << "Check bind without root isolation" << std::endl;
    string path = config().container().tmp_dir() + "/" + name;

    TFolder tmp("/tmp/27389");
    if (tmp.Exists())
        ExpectSuccess(tmp.Remove(true));
    ExpectSuccess(tmp.Create(0755, true));

    TFile f(tmp.GetPath() + "/porto");
    ExpectSuccess(f.Touch());

    ExpectSuccess(api.SetProperty(name, "command", "cat /proc/self/mountinfo"));
    ExpectSuccess(api.SetProperty(name, "bind", "/bin /bin ro; /tmp/27389 /tmp"));
    string v = StartWaitAndGetData(api, name, "stdout");
    ExpectSuccess(api.Stop(name));

    auto m = ParseMountinfo(v);
    Expect(m[path + "/bin"] == "ro,relatime");
    Expect(m[path + "/tmp"] == "rw,relatime");

    AsRoot(api);
    BootstrapCommand("/bin/cat", path);
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    ExpectSuccess(api.SetProperty(name, "root", path));
    ExpectSuccess(api.SetProperty(name, "bind", "/bin /bin ro; /tmp/27389 /tmp"));
    v = StartWaitAndGetData(api, name, "stdout");
    ExpectSuccess(api.Stop(name));

    m = ParseMountinfo(v);
    Expect(m["/bin"] == "ro,relatime");
    Expect(m["/tmp"] == "rw,relatime");

    ExpectSuccess(api.Destroy(name));
}

static vector<string> StringToVec(const std::string &s) {
    vector<string> lines;

    TError error = SplitString(s, '\n', lines);
    if (error)
        throw error.GetMsg();
    return lines;
}

static void TestNetProperty(TPortoAPI &api) {
    const size_t linesPerDev = 2;
    string name = "a";
    string path = config().container().tmp_dir() + "/" + name;
    ExpectSuccess(api.Create(name));

    vector<string> hostLink = Popen("ip link show");

    Say() << "Check net parsing" << std::endl;
    ExpectFailure(api.SetProperty(name, "net", "qwerty"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "net", "host"));
    ExpectSuccess(api.SetProperty(name, "net", "none"));
    //ExpectSuccess(api.SetProperty(name, "net", "host; macvlan X Y"));
    ExpectFailure(api.SetProperty(name, "net", "host; host"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "host; none"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "none; host"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "macvlan"), EError::NotSupported);
    ExpectFailure(api.SetProperty(name, "net", "host: veth0"), EError::NotSupported);

    Say() << "Check net=none" << std::endl;
    AsRoot(api);
    BootstrapCommand("/bin/ip", path);
    AsNobody(api);
    ExpectSuccess(api.SetProperty(name, "root", path));
    ExpectSuccess(api.SetProperty(name, "net", "none"));
    ExpectSuccess(api.SetProperty(name, "command", "/ip link show"));
    string s = StartWaitAndGetData(api, name, "stdout");
    auto v = StringToVec(s);
    ExpectSuccess(api.Stop(name));

    Expect(v.size() == 1 * linesPerDev);
    Expect(v.size() != hostLink.size());

    Say() << "Check net=host" << std::endl;
    ExpectSuccess(api.SetProperty(name, "net", "host"));
    s = StartWaitAndGetData(api, name, "stdout");
    v = StringToVec(s);
    ExpectSuccess(api.Stop(name));

    Expect(v.size() == hostLink.size());

    Say() << "Check net=host:veth0" << std::endl;

    // TODO: create veth and pass it to network

    ExpectSuccess(api.Destroy(name));
}

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

    Say() << "Make sure we can stop unintentionally frozen container " << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    v = GetFreezer(name);
    Expect(v == "THAWED\n");

    AsRoot(api);
    SetFreezer(name, "FROZEN");
    AsNobody(api);

    v = GetFreezer(name);
    Expect(v == "FROZEN\n");

    ExpectSuccess(api.Stop(name));

    Say() << "Make sure we can remove paused container " << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.Pause(name));
    ExpectSuccess(api.Destroy(name));

    Say() << "Make sure kill works " << std::endl;
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    // if container init process doesn't have custom handler for a signal
    // it's ignored
    ExpectSuccess(api.Kill(name, SIGTERM));
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "running");

    ExpectSuccess(api.Kill(name, SIGKILL));
    ExpectSuccess(api.GetData(name, "root_pid", v));
    WaitExit(api, v);
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "dead");

    // we can't kill root or non-running container
    ExpectFailure(api.Kill(name, SIGKILL), EError::InvalidState);
    ExpectFailure(api.Kill("/", SIGKILL), EError::InvalidState);

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
        "recharge_on_pgfault",
        "cpu_policy",
        "cpu_priority",
        "net_guarantee",
        "net_ceil",
        "net_priority",
        "respawn",
        "isolate",
        "stdin_path",
        "stdout_path",
        "stderr_path",
        "stdout_limit",
        "private",
        "ulimit",
        "hostname",
        "root",
        "bind_dns",
        "max_respawns",
        "bind",
        "net",
    };

    std::vector<TProperty> plist;

    ExpectSuccess(api.Plist(plist));
    Expect(plist.size() == properties.size());

    Say() << "Check root properties & data" << std::endl;
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

    Say() << "Check root cpu_usage & memory_usage" << std::endl;
    ExpectSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "memory_usage", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_bytes", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_packets", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_drops", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_overlimits", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "io_read", v));
    Expect(v == "");
    ExpectSuccess(api.GetData(root, "io_write", v));
    Expect(v == "");

    uint32_t defClass = TcHandle(1, 2);
    uint32_t rootClass = TcHandle(1, 1);
    uint32_t nextClass = TcHandle(1, 3);

    uint32_t rootQdisc = TcHandle(1, 0);
    uint32_t nextQdisc = TcHandle(2, 0);

    TNetlink nl;
    Expect(nl.Open(config().network().device()) == TError::Success());
    Expect(nl.QdiscExists(rootQdisc) == true);
    Expect(nl.QdiscExists(nextQdisc) == false);
    Expect(nl.ClassExists(defClass) == true);
    Expect(nl.ClassExists(rootClass) == true);
    Expect(nl.ClassExists(nextClass) == false);

    Expect(nl.CgroupFilterExists(rootQdisc, 1) == true);
    Expect(nl.CgroupFilterExists(rootQdisc, 2) == false);
}

static void TestStats(TPortoAPI &api) {
    // should be executed right after TestRoot because assumes empty statistics

    string root = "/";
    string wget = "wget";
    string noop = "noop";

    ExpectSuccess(api.Create(noop));
    // this will cause io read and noop will not have io_read
    Expect(system("ls -la /bin >/dev/null") == 0);
    ExpectSuccess(api.SetProperty(noop, "command", "ls -la /bin"));
    ExpectSuccess(api.Start(noop));
    WaitState(api, noop, "dead");

    ExpectSuccess(api.Create(wget));
    ExpectSuccess(api.SetProperty(wget, "command", "bash -c 'wget yandex.ru && sync'"));
    ExpectSuccess(api.Start(wget));
    WaitState(api, wget, "dead");

    string v, rv;
    ExpectSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(root, "memory_usage", v));
    Expect(v != "0" && v != "-1");

    if(IsCfqActive()) {
        ExpectSuccess(api.GetData(root, "io_write", v));
        Expect(v != "");
        ExpectSuccess(api.GetData(root, "io_read", v));
        Expect(v != "");
    }
    ExpectSuccess(api.GetData(wget, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(wget, "memory_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(wget, "io_write", v));
    Expect(v != "");
    ExpectSuccess(api.GetData(wget, "io_read", v));
    Expect(v != "");

    ExpectSuccess(api.GetData(noop, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(noop, "memory_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(noop, "io_write", v));
    Expect(v == "");
    ExpectSuccess(api.GetData(noop, "io_read", v));
    Expect(v == "");

    ExpectSuccess(api.GetData(root, "net_bytes", rv));
    Expect(rv != "0" && rv != "-1");
    ExpectSuccess(api.GetData(wget, "net_bytes", v));
    Expect(v == rv);
    ExpectSuccess(api.GetData(noop, "net_bytes", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_packets", rv));
    Expect(rv != "0" && rv != "-1");
    ExpectSuccess(api.GetData(wget, "net_packets", v));
    Expect(v == rv);
    ExpectSuccess(api.GetData(noop, "net_packets", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_drops", rv));
    Expect(rv == "0");
    ExpectSuccess(api.GetData(wget, "net_drops", v));
    Expect(v == rv);
    ExpectSuccess(api.GetData(noop, "net_drops", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "net_overlimits", rv));
    Expect(rv == "0");
    ExpectSuccess(api.GetData(wget, "net_overlimits", v));
    Expect(v == rv);
    ExpectSuccess(api.GetData(noop, "net_overlimits", v));
    Expect(v == "0");

    ExpectSuccess(api.Destroy(wget));
    ExpectSuccess(api.Destroy(noop));
}

static bool CanTestLimits() {
    if (!HaveCgKnob("memory", "memory.low_limit_in_bytes"))
        return false;

    if (!HaveCgKnob("memory", "memory.recharge_on_pgfault"))
        return false;

    if (!HaveCgKnob("cpu", "cpu.smart"))
        return false;

    return true;
}

static void TestLimits(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check default limits" << std::endl;
    string current;

    current = GetCgKnob("memory", "", "memory.use_hierarchy");
    Expect(current == "1");

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.use_hierarchy");
    Expect(current == "1");

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX));

    if (HaveCgKnob("memory", "memory.low_limit_in_bytes")) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        Expect(current == "0");
    }
    ExpectSuccess(api.Stop(name));

    Say() << "Check custom limits" << std::endl;
    string exp_limit = "524288";
    string exp_guar = "16384";
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    if (HaveCgKnob("memory", "memory.low_limit_in_bytes"))
        ExpectSuccess(api.SetProperty(name, "memory_guarantee", exp_guar));
    ExpectSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == exp_limit);
    if (HaveCgKnob("memory", "memory.low_limit_in_bytes")) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        Expect(current == exp_guar);
    }
    ExpectSuccess(api.Stop(name));

    Say() << "Check cpu_priority" << std::endl;
    ExpectFailure(api.SetProperty(name, "cpu_priority", "-1"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "cpu_priority", "100"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "cpu_priority", "0"));
    ExpectSuccess(api.SetProperty(name, "cpu_priority", "99"));

    Say() << "Check cpu_policy" << std::endl;
    string smart;

    ExpectFailure(api.SetProperty(name, "cpu_policy", "somecrap"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "cpu_policy", "idle"), EError::NotSupported);

    if (HaveCgKnob("cpu", "cpu.smart")) {
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

    uint32_t netGuarantee = 100000, netCeil = 200000, netPrio = 4;
    ExpectSuccess(api.SetProperty(name, "net_guarantee", std::to_string(netGuarantee)));
    ExpectSuccess(api.SetProperty(name, "net_ceil", std::to_string(netCeil)));
    ExpectFailure(api.SetProperty(name, "net_priority", "-1"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net_priority", "8"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "net_priority", "0"));
    ExpectSuccess(api.SetProperty(name, "net_priority", std::to_string(netPrio)));
    ExpectSuccess(api.Start(name));

    uint32_t prio, rate, ceil;
    TNetlink nl;
    Expect(nl.Open(config().network().device()) == TError::Success());
    string handle = GetCgKnob("net_cls", name, "net_cls.classid");
    ExpectSuccess(nl.GetClassProperties(stoul(handle), prio, rate, ceil));

    Expect(prio == netPrio);
    Expect(rate == netGuarantee);
    Expect(ceil == netCeil);
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestRlimits(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check rlimits parsing" << std::endl;

    ExpectSuccess(api.SetProperty(name, "ulimit", ""));
    ExpectFailure(api.SetProperty(name, "ulimit", "qwe"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "ulimit", "qwe: 123"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "ulimit", "qwe: 123 456"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "ulimit", "as: 123"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "ulimit", "as 123 456"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "ulimit", "as: 123 456 789"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "ulimit", "as: 123 :456"), EError::InvalidValue);

    Say() << "Check rlimits" << std::endl;

    map<string, pair<string, string>> rlim = {
        { "nproc", { "20480", "30720" } },
        { "nofile", { "819200", "1024000" } },
        { "data", { "8388608000", "10485760000" } },
        { "memlock", { "41943040000", "41943040000" } },
    };

    string ulimit;
    for (auto &lim : rlim) {
        if (ulimit.length())
            ulimit += "; ";

        ulimit += lim.first + ": " + lim.second.first + " " + lim.second.second;
    }

    ExpectSuccess(api.SetProperty(name, "ulimit", ulimit));
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    string pid;
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    AsRoot(api);

    for (auto &lim : rlim) {
        Expect(GetRlimit(pid, lim.first, true) == lim.second.first);
        Expect(GetRlimit(pid, lim.first, false) == lim.second.second);
    }

    ExpectSuccess(api.Destroy(name));
}

static void TestAlias(TPortoAPI &api) {
    if (!HaveCgKnob("memory", "memory.low_limit_in_bytes"))
        return;
    if (!HaveCgKnob("memory", "memory.recharge_on_pgfault"))
        return;
    if (!HaveCgKnob("cpu", "cpu.smart"))
        return;

    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Check default limits" << std::endl;
    string current;
    string alias, real;

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectSuccess(api.GetProperty(name, "memory_limit", real));
    Expect(alias == real);
    ExpectSuccess(api.GetProperty(name, "memory.low_limit_in_bytes", alias));
    ExpectSuccess(api.GetProperty(name, "memory_guarantee", real));
    Expect(alias == real);
    ExpectSuccess(api.GetProperty(name, "memory.recharge_on_pgfault", alias));
    ExpectSuccess(api.GetProperty(name, "recharge_on_pgfault", real));
    Expect(alias == "0");
    Expect(real == "false");
    ExpectSuccess(api.GetProperty(name, "cpu.smart", alias));
    ExpectSuccess(api.GetProperty(name, "cpu_policy", real));
    Expect(alias == "0");
    Expect(real == "normal");
    ExpectSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX));

    current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
    Expect(current == "0");

    current = GetCgKnob("memory", name, "memory.recharge_on_pgfault");
    Expect(current == "0");

    current = GetCgKnob("cpu", name, "cpu.smart");
    Expect(current == "0");
    ExpectSuccess(api.Stop(name));

    Say() << "Check custom limits" << std::endl;
    string exp_limit = "524288";
    string exp_guar = "16384";
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));

    ExpectSuccess(api.SetProperty(name, "memory.limit_in_bytes", "1"));
    ExpectSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    Expect(alias == "1");
    ExpectSuccess(api.SetProperty(name, "memory.limit_in_bytes", "1k"));
    ExpectSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    Expect(alias == "1024");
    ExpectSuccess(api.SetProperty(name, "memory.limit_in_bytes", "12m"));
    ExpectSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    Expect(alias == "12582912");
    ExpectSuccess(api.SetProperty(name, "memory.limit_in_bytes", "123g"));
    ExpectSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    Expect(alias == "132070244352");

    ExpectSuccess(api.SetProperty(name, "memory.limit_in_bytes", exp_limit));
    ExpectSuccess(api.SetProperty(name, "memory.low_limit_in_bytes", exp_guar));
    ExpectSuccess(api.SetProperty(name, "memory.recharge_on_pgfault", "1"));
    ExpectSuccess(api.SetProperty(name, "cpu.smart", "1"));

    ExpectSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectSuccess(api.GetProperty(name, "memory_limit", real));
    Expect(alias == real);
    ExpectSuccess(api.GetProperty(name, "memory.low_limit_in_bytes", alias));
    ExpectSuccess(api.GetProperty(name, "memory_guarantee", real));
    Expect(alias == real);
    ExpectSuccess(api.GetProperty(name, "memory.recharge_on_pgfault", alias));
    ExpectSuccess(api.GetProperty(name, "recharge_on_pgfault", real));
    Expect(alias == "1");
    Expect(real == "true");
    ExpectSuccess(api.GetProperty(name, "cpu.smart", alias));
    ExpectSuccess(api.GetProperty(name, "cpu_policy", real));
    Expect(alias == "1");
    Expect(real == "rt");

    ExpectSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == exp_limit);
    current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
    Expect(current == exp_guar);

    current = GetCgKnob("memory", name, "memory.recharge_on_pgfault");
    Expect(current == "1");

    current = GetCgKnob("cpu", name, "cpu.smart");
    Expect(current == "1");
    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.Destroy(name));
}

static void TestDynamic(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    string current;
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX));

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
    if (!HaveCgKnob("memory", "memory.low_limit_in_bytes"))
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

    Say() << "Single container can't go over reserve" << std::endl;
    ExpectFailure(api.SetProperty(system, "memory_guarantee", std::to_string(total)), EError::ResourceNotAvailable);
    ExpectSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(total - config().daemon().memory_guarantee_reserve())));

    Say() << "Distributed guarantee can't go over reserve" << std::endl;
    size_t chunk = (total - config().daemon().memory_guarantee_reserve()) / 4;

    ExpectSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(chunk)));
    ExpectSuccess(api.SetProperty(monit, "memory_guarantee", std::to_string(chunk)));
    ExpectSuccess(api.SetProperty(slot1, "memory_guarantee", std::to_string(chunk)));
    ExpectFailure(api.SetProperty(slot2, "memory_guarantee", std::to_string(chunk + 1)), EError::ResourceNotAvailable);
    ExpectSuccess(api.SetProperty(slot2, "memory_guarantee", std::to_string(chunk)));

    ExpectSuccess(api.SetProperty(monit, "memory_guarantee", std::to_string(0)));
    ExpectSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(0)));

    auto CheckPropertyHierarhcy = [&](TPortoAPI &api, const std::string &property) {
        Say() << "Parent can't have less guarantee than sum of children" << std::endl;
        ExpectSuccess(api.SetProperty(slot1, property, std::to_string(chunk)));
        ExpectSuccess(api.SetProperty(slot2, property, std::to_string(chunk)));
        ExpectFailure(api.SetProperty(prod, property, std::to_string(chunk)), EError::InvalidValue);
        ExpectFailure(api.SetProperty(box, property, std::to_string(chunk)), EError::InvalidValue);

        Say() << "Child can't go over parent guarantee" << std::endl;
        ExpectSuccess(api.SetProperty(prod, property, std::to_string(2 * chunk)));
        ExpectFailure(api.SetProperty(slot1, property, std::to_string(2 * chunk)), EError::InvalidValue);

        Say() << "Can lower guarantee if possible" << std::endl;
        ExpectFailure(api.SetProperty(prod, property, std::to_string(chunk)), EError::InvalidValue);
        ExpectSuccess(api.SetProperty(slot2, property, std::to_string(0)));
        ExpectSuccess(api.SetProperty(prod, property, std::to_string(chunk)));
    };

    CheckPropertyHierarhcy(api, "memory_guarantee");
    CheckPropertyHierarhcy(api, "memory_limit");

    ExpectSuccess(api.Destroy(monit));
    ExpectSuccess(api.Destroy(system));
    ExpectSuccess(api.Destroy(slot2));
    ExpectSuccess(api.Destroy(slot1));
    ExpectSuccess(api.Destroy(prod));
    ExpectSuccess(api.Destroy(box));

    Say() << "Test child-parent isolation" << std::endl;

    string parent = "parent";
    string child = "parent/child";

    ExpectSuccess(api.Create(parent));
    ExpectSuccess(api.SetProperty(parent, "command", "sleep 1000"));
    ExpectSuccess(api.Start(parent));

    ExpectSuccess(api.Create(child));
    ExpectSuccess(api.SetProperty(child, "isolate", "false"));
    ExpectSuccess(api.SetProperty(child, "command", "sleep 1000"));

    string exp_limit = "268435456";
    ExpectSuccess(api.SetProperty(child, "memory_limit", exp_limit));
    ExpectFailure(api.SetProperty(child, "memory_guarantee", "10000"), EError::NotSupported);
    ExpectSuccess(api.SetProperty(child, "respawn", "true"));

    ExpectSuccess(api.Start(child));

    string current = GetCgKnob("memory", child, "memory.limit_in_bytes");
    Expect(current == exp_limit);
    current = GetCgKnob("memory", parent, "memory.limit_in_bytes");
    Expect(current != exp_limit);

    string parentProperty, childProperty;
    ExpectSuccess(api.GetProperty(parent, "stdout_path", parentProperty));
    ExpectSuccess(api.GetProperty(child, "stdout_path", childProperty));
    Expect(parentProperty != childProperty);
    ExpectSuccess(api.GetProperty(parent, "stderr_path", parentProperty));
    ExpectSuccess(api.GetProperty(child, "stderr_path", childProperty));
    Expect(parentProperty != childProperty);

    string parentPid, childPid;

    ExpectSuccess(api.GetData(parent, "root_pid", parentPid));
    ExpectSuccess(api.GetData(child, "root_pid", childPid));

    AsRoot(api);

    auto parentCgmap = GetCgroups(parentPid);
    auto childCgmap = GetCgroups(childPid);

    Expect(parentCgmap["freezer"] != childCgmap["freezer"]);
    Expect(parentCgmap["memory"] != childCgmap["memory"]);
    Expect(parentCgmap["net_cls"] == childCgmap["net_cls"]);
    Expect(parentCgmap["cpu"] == childCgmap["cpu"]);
    Expect(parentCgmap["cpuact"] == childCgmap["cpuact"]);

    Expect(GetCwd(parentPid) == GetCwd(childPid));

    for (auto &ns : namespaces)
        Expect(GetNamespace(parentPid, ns) == GetNamespace(childPid, ns));

    ExpectSuccess(api.Destroy(child));
    ExpectSuccess(api.Destroy(parent));
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

    Say() << "Only user that created container can start/stop/destroy/etc it" << std::endl;

    TUser daemonUser("daemon");
    TError error = daemonUser.Load();
    if (error)
        throw error.GetMsg();

    TGroup daemonGroup("daemon");
    error = daemonGroup.Load();
    if (error)
        throw error.GetMsg();

    TUser binUser("bin");
    error = binUser.Load();
    if (error)
        throw error.GetMsg();

    TGroup binGroup("bin");
    error = binGroup.Load();
    if (error)
        throw error.GetMsg();

    string s;

    AsUser(api, daemonUser, daemonGroup);
           ExpectSuccess(api.Create(name));

    AsUser(api, binUser, binGroup);
           ExpectFailure(api.Start(name), EError::Permission);
           ExpectFailure(api.Destroy(name), EError::Permission);
           ExpectFailure(api.SetProperty(name, "command", "sleep 1000"), EError::Permission);
           ExpectFailure(api.GetProperty(name, "command", s), EError::Permission);

    AsUser(api, daemonUser, daemonGroup);
        ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectFailure(api.SetProperty(name, "user", "mail"), EError::Permission);
        ExpectFailure(api.SetProperty(name, "group", "mail"), EError::Permission);
        ExpectSuccess(api.GetProperty(name, "command", s));
        ExpectSuccess(api.Start(name));
        ExpectSuccess(api.GetData(name, "root_pid", s));

    AsUser(api, binUser, binGroup);
        ExpectFailure(api.GetData(name, "root_pid", s), EError::Permission);
        ExpectFailure(api.Stop(name), EError::Permission);
        ExpectFailure(api.Pause(name), EError::Permission);

    AsUser(api, daemonUser, daemonGroup);
        ExpectSuccess(api.Pause(name));

    AsUser(api, binUser, binGroup);
        ExpectFailure(api.Destroy(name), EError::Permission);
        ExpectFailure(api.Resume(name), EError::Permission);

    AsRoot(api);
    ExpectSuccess(api.Destroy(name));
    AsNobody(api);
}

static void TestRespawn(TPortoAPI &api) {
    string pid, respawnPid;
    string ret;

    string name = "a";
    ExpectSuccess(api.Create(name));
    ExpectFailure(api.SetProperty(name, "max_respawns", "true"), EError::InvalidValue);

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1"));

    ExpectSuccess(api.SetProperty(name, "respawn", "false"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "respawn_count", ret));
    Expect(ret == string("0"));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "respawn_count", ret));
    Expect(ret == string("0"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.Start(name));

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "root_pid", respawnPid));
    Expect(pid != respawnPid);
    ExpectSuccess(api.GetData(name, "respawn_count", ret));
    Expect(ret != "0" && ret != "");

    ExpectSuccess(api.Stop(name));

    string realRespawns;
    string maxRespawns = "3";
    int successRespawns = 0;
    int maxTries = 10;

    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.SetProperty(name, "max_respawns", maxRespawns));
    ExpectSuccess(api.SetProperty(name, "command", "echo test"));
    ExpectSuccess(api.Start(name));

    for(int i = 0; i < maxTries; i++) {
        sleep(config().daemon().heartbeat_delay_ms() / 1000);
        api.GetData(name, "respawn_count", realRespawns);
        if (realRespawns == maxRespawns)
            successRespawns++;
        if (successRespawns == 2)
            break;
        Say() << "Respawned " << i << " times" << std::endl;
    }
    Expect(maxRespawns == realRespawns);

    ExpectSuccess(api.Destroy(name));
}

static int LeakConainersNr;
static void TestLeaks(TPortoAPI &api) {
    string pid;
    string name;
    int slack = 4096;

    TFile f(config().slave_pid().path());
    Expect(f.AsString(pid) == false);

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ExpectSuccess(api.Create(name));
        ExpectSuccess(api.SetProperty(name, "command", "true"));
        ExpectSuccess(api.Start(name));
    }

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ExpectSuccess(api.Destroy(name));
    }

    int prev = GetVmRss(pid);

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "b" + std::to_string(i);
        ExpectSuccess(api.Create(name));
        ExpectSuccess(api.SetProperty(name, "command", "true"));
        ExpectSuccess(api.Start(name));
    }

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "b" + std::to_string(i);
        ExpectSuccess(api.Destroy(name));
    }

    int now = GetVmRss(pid);
    Say() << "Expected " << now << " < " << prev + slack << std::endl;
    Expect(now <= prev + slack);
}

static void TestRecovery(TPortoAPI &api) {
    string pid, v;
    string name = "a:b";

    map<string,string> props = {
        { "command", "sleep 1000" },
        { "user", "bin" },
        { "group", "daemon" },
        { "env", "a=a;b=b" },
    };

    Say() << "Make sure we don't kill containers when doing recovery" << std::endl;

    AsRoot(api);
    ExpectSuccess(api.Create(name));

    for (auto &pair : props)
        ExpectSuccess(api.SetProperty(name, pair.first, pair.second));
    ExpectSuccess(api.Start(name));

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(TaskRunning(api, pid) == true);
    Expect(TaskZombie(api, pid) == false);

    int portodPid = ReadPid(config().slave_pid().path());
    if (kill(portodPid, SIGKILL))
        throw string("Can't send SIGKILL to slave");

    WaitExit(api, std::to_string(portodPid));
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
    AsNobody(api);

    Say() << "Make sure hierarchical recovery works" << std::endl;

    string parent = "a";
    string child = "a/b";
    ExpectSuccess(api.Create(parent));
    ExpectSuccess(api.Create(child));

    portodPid = ReadPid(config().slave_pid().path());
    AsRoot(api);
    if (kill(portodPid, SIGKILL))
        throw string("Can't send SIGKILL to slave");
    WaitExit(api, std::to_string(portodPid));
    WaitPortod(api);
    AsNobody(api);

    std::vector<std::string> containers;

    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("a/b"));
    ExpectSuccess(api.Destroy(child));
    ExpectSuccess(api.Destroy(parent));
}

static void TestCgroups(TPortoAPI &api) {
    string cg = "/sys/fs/cgroup/freezer/qwerty/asdfg";

    AsRoot(api);

    TFolder f(cg);
    if (f.Exists())
        Expect(f.Remove() == false);
    Expect(f.Create(0755, true) == false);

    int pid = ReadPid(config().slave_pid().path());
    if (kill(pid, SIGINT))
        throw string("Can't send SIGINT to slave");

    WaitPortod(api);

    pid = ReadPid(config().slave_pid().path());
    if (kill(pid, SIGINT))
        throw string("Can't send SIGINT to slave");

    WaitPortod(api);

    Expect(f.Exists() == true);
    Expect(f.Remove() == false);
}

int SelfTest(string name, int leakNr) {
    pair<string, std::function<void(TPortoAPI &)>> tests[] = {
        { "root", TestRoot },
        { "stats", TestStats },
        { "holder", TestHolder },
        { "empty", TestEmpty },
        { "state_machine", TestStateMachine },
        { "exit_status", TestExitStatus },
        { "streams", TestStreams },
        { "long_running", TestLongRunning },
        { "isolation", TestIsolation },
        { "property", TestProperty },
        { "environment", TestEnvironment },
        { "user_group", TestUserGroup },
        { "cwd", TestCwd },
        { "std", TestStd },
        { "root_property", TestRootProperty },
        { "hostname_property", TestHostnameProperty },
        { "bind_property", TestBindProperty },
        { "net_property", TestNetProperty },
        { "limits", TestLimits },
        { "rlimits", TestRlimits },
        { "alias", TestAlias },
        { "dynamic", TestDynamic },
        { "permissions", TestPermissions },
        { "respawn", TestRespawn },
        { "hierarchy", TestLimitsHierarchy },
        { "leaks", TestLeaks },

        { "daemon", TestDaemon },
        { "recovery", TestRecovery },
        { "cgroups", TestCgroups },
    };

    LeakConainersNr = leakNr;

    int respawns = 0;
    int errors = 0;
    try {
        config.Load();
        TPortoAPI api(config().rpc_sock().file().path());

        RestartDaemon(api);

        Expect(WordCount(config().master_log().path(), "Started") == 1);
        Expect(WordCount(config().slave_log().path(), "Started") == 1);

        TGroup portoGroup("porto");
        TError error = portoGroup.Load();
        if (error)
            throw error.GetMsg();

        gid_t portoGid = portoGroup.GetId();
        Expect(setgroups(1, &portoGid) == 0);

        for (auto t : tests) {
            if (name.length() && name != t.first)
                continue;

            std::cerr << ">>> Testing " << t.first << "..." << std::endl;
            AsNobody(api);

            t.second(api);
        }

        respawns = WordCount(config().master_log().path(), "Spawned");
        errors = WordCount(config().slave_log().path(), "Error");
    } catch (string e) {
        std::cerr << "EXCEPTION: " << e << std::endl;
        return 1;
    }

    std::cerr << "SUCCESS: All tests successfully passed!" << std::endl;
    if (WordCount(config().slave_log().path(), "Task belongs to invalid subsystem"))
        std::cerr << "WARNING: Some task belongs to invalid subsystem!" << std::endl;
    if (!CanTestLimits())
        std::cerr << "WARNING: Due to missing kernel support, memory_guarantee/cpu_policy has not been tested!" << std::endl;
    if (respawns != 1 /* start */ + 2 /* TestRecovery */ + 2 /* TestCgroups */)
        std::cerr << "WARNING: Unexpected number of respawns: " << respawns << "!" << std::endl;
    if (errors !=
        4 + /* Invalid command */
        6 + /* Invalid property value */
        2 + /* Memory guarantee */
        8 + /* Hierarchical properties */
        1 + /* respawn */
        7 + /* ulimit */
        4 + /* bind */
        6 + /* net */
        3 /* Can't remove cgroups */)
        std::cerr << "WARNING: Unexpected number of errors: " << errors << "!" << std::endl;

    if (!IsCfqActive())
        std::cerr << "WARNING: CFQ is not enabled for one of your block devices, skipping io_read and io_write tests" << std::endl;

    return 0;
}
}
