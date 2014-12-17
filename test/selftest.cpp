#include <iostream>
#include <iomanip>
#include <csignal>
#include <cstdio>
#include <algorithm>

#include "rpc.pb.h"
#include "libporto.hpp"
#include "config.hpp"
#include "util/netlink.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/pwd.hpp"
#include "test.hpp"

#define HOSTNAME "portotest"
const std::string TMPDIR = "/tmp/porto/selftest";

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <linux/capability.h>
}

const uint32_t DEF_CLASS_PRIO = 50;

const uint32_t DEF_CLASS_MAX_RATE = -1;
const uint32_t DEF_CLASS_RATE = -1;
const uint32_t DEF_CLASS_CEIL = DEF_CLASS_MAX_RATE;
const uint32_t DEF_CLASS_NET_PRIO = 3;

using std::string;
using std::vector;
using std::map;
using std::pair;

namespace test {

static int expectedErrors;
static int expectedRespawns;
static int expectedWarns;
static bool needDaemonChecks;

static vector<string> subsystems = { "freezer", "memory", "cpu", "cpuacct", "devices" };
static vector<string> namespaces = { "pid", "mnt", "ipc", "net", /*"user", */"uts" };

static int LeakConainersNr;

static void RemakeDir(TPortoAPI &api, const TPath &path) {
    bool drop = geteuid() != 0;
    TFolder f(path);
    if (drop)
        AsRoot(api);
    if (f.Exists())
        ExpectSuccess(f.Remove(true));
    if (drop)
        AsNobody(api);
    ExpectSuccess(f.Create(0755, true));
}

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

    for (auto &link : links) {
        ExpectSuccess(api.GetProperty(name, "net_guarantee[" + link->GetAlias() + "]", v));
        Expect(v == std::to_string(DEF_CLASS_RATE));
        ExpectSuccess(api.GetProperty(name, "net_ceil[" + link->GetAlias() + "]", v));
        Expect(v == std::to_string(DEF_CLASS_CEIL));
        ExpectSuccess(api.GetProperty(name, "net_priority[" + link->GetAlias() + "]", v));
        Expect(v == std::to_string(DEF_CLASS_NET_PRIO));
        ExpectSuccess(api.GetProperty(name, "net", v));
        Expect(v == "host");
    }

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
    ExpectSuccess(api.GetProperty(name, "allowed_devices", v));
    Expect(v == "a *:* rwm");
    ExpectSuccess(api.GetProperty(name, "capabilities", v));
    Expect(v == "");
    ExpectSuccess(api.GetProperty(name, "recharge_on_pgfault", v));
    Expect(v == "false");
    ExpectSuccess(api.GetProperty(name, "isolate", v));
    Expect(v == "true");
    ExpectSuccess(api.GetProperty(name, "stdout_limit", v));
    Expect(v == std::to_string(config().container().stdout_limit()));
    ExpectSuccess(api.GetProperty(name, "private", v));
    Expect(v == "");
    ExpectSuccess(api.GetProperty(name, "bind", v));
    Expect(v == "");
    ExpectSuccess(api.GetProperty(name, "root_readonly", v));
    Expect(v == "false");
    ExpectSuccess(api.GetProperty(name, "max_respawns", v));
    Expect(v == "-1");
}

static void ShouldHaveValidRunningData(TPortoAPI &api, const string &name) {
    string v;

    ExpectFailure(api.GetData(name, "__invalid_data__", v), EError::InvalidData);

    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == string("running"));
    ExpectFailure(api.GetData(name, "exit_status", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "start_errno", v), EError::InvalidState);

    ExpectSuccess(api.GetData(name, "root_pid", v));
    Expect(v != "" && v != "-1" && v != "0");

    ExpectSuccess(api.GetData(name, "stdout", v));
    ExpectSuccess(api.GetData(name, "stderr", v));
    ExpectSuccess(api.GetData(name, "cpu_usage", v));
    ExpectSuccess(api.GetData(name, "memory_usage", v));

    if (NetworkEnabled()) {
        ExpectSuccess(api.GetData(name, "net_bytes", v));
        ExpectSuccess(api.GetData(name, "net_packets", v));
        ExpectSuccess(api.GetData(name, "net_drops", v));
        ExpectSuccess(api.GetData(name, "net_overlimits", v));
    }
    ExpectSuccess(api.GetData(name, "minor_faults", v));
    ExpectSuccess(api.GetData(name, "major_faults", v));

    ExpectFailure(api.GetData(name, "oom_killed", v), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "respawn_count", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetData(name, "parent", v));
    Expect(v == string("/"));
    if (IsCfqActive()) {
        ExpectSuccess(api.GetData(name, "io_read", v));
        ExpectSuccess(api.GetData(name, "io_write", v));
    }
}

static void ShouldHaveValidData(TPortoAPI &api, const string &name) {
    string v;

    ExpectFailure(api.GetData(name, "__invalid_data__", v), EError::InvalidData);

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

    if (NetworkEnabled()) {
        ExpectFailure(api.GetData(name, "net_bytes", v), EError::InvalidState);
        ExpectFailure(api.GetData(name, "net_packets", v), EError::InvalidState);
        ExpectFailure(api.GetData(name, "net_drops", v), EError::InvalidState);
        ExpectFailure(api.GetData(name, "net_overlimits", v), EError::InvalidState);
    }
    ExpectFailure(api.GetData(name, "minor_faults", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "major_faults", v), EError::InvalidState);

    ExpectFailure(api.GetData(name, "oom_killed", v), EError::InvalidState);
    ExpectFailure(api.GetData(name, "respawn_count", v), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "parent", v));
    Expect(v == string("/"));
    if (IsCfqActive()) {
        ExpectFailure(api.GetData(name, "io_read", v), EError::InvalidState);
        ExpectFailure(api.GetData(name, "io_write", v), EError::InvalidState);
    }
    ExpectSuccess(api.GetProperty(name, "max_respawns", v));
    Expect(v == "-1");
}

static void ExpectTclass(string name, bool exp) {
    string cls = GetCgKnob("net_cls", name, "net_cls.classid");
    Expect(TcClassExist(stoul(cls)) == exp);
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
    ExpectSuccess(api.Destroy(parent));
    string v;
    ExpectFailure(api.GetData(child, "state", v), EError::ContainerDoesNotExist);
    ExpectFailure(api.GetData(parent, "state", v), EError::ContainerDoesNotExist);

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

    Say() << "Make sure parent gets valid state when child starts" << std::endl;

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

    ExpectSuccess(api.Start("a/b/c"));
    ExpectSuccess(api.GetData("a/b/c", "state", v));
    Expect(v == "running");
    ExpectSuccess(api.GetData("a/b", "state", v));
    Expect(v == "meta");
    ExpectSuccess(api.GetData("a", "state", v));
    Expect(v == "meta");
    ExpectSuccess(api.Stop("a/b/c"));

    ExpectSuccess(api.Start("a/b"));
    ExpectSuccess(api.GetData("a/b/c", "state", v));
    Expect(v == "stopped");
    ExpectSuccess(api.GetData("a/b", "state", v));
    Expect(v == "running");
    ExpectSuccess(api.GetData("a", "state", v));
    Expect(v == "meta");
    ExpectSuccess(api.Stop("a/b"));

    ExpectSuccess(api.Start("a"));
    ExpectSuccess(api.GetData("a/b/c", "state", v));
    Expect(v == "stopped");
    ExpectSuccess(api.GetData("a/b", "state", v));
    Expect(v == "stopped");
    ExpectSuccess(api.GetData("a", "state", v));
    Expect(v == "running");
    ShouldHaveValidRunningData(api, "a");
    ExpectSuccess(api.Stop("a"));

    Say() << "Make sure when parent stops/dies children are stopped" << std::endl;

    string state;

    ExpectSuccess(api.Start("a"));
    ExpectSuccess(api.Start("a/b"));
    ExpectSuccess(api.Start("a/b/c"));

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

    if (NetworkEnabled()) {
        ExpectTclass("a", true);
        ExpectTclass("a/b", true);
        ExpectTclass("a/b/c", true);
    }

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

static void TestNsCgTc(TPortoAPI &api) {
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
    Expect(GetNamespace("self", "ipc") != GetNamespace(pid, "ipc"));
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

    string root_cls;
    string leaf_cls;
    if (NetworkEnabled()) {
        root_cls = GetCgKnob("net_cls", "/", "net_cls.classid");
        leaf_cls = GetCgKnob("net_cls", name, "net_cls.classid");

        Expect(root_cls != "0");
        Expect(leaf_cls != "0");
        Expect(root_cls != leaf_cls);

        Expect(TcClassExist(stoul(root_cls)) == true);
        Expect(TcClassExist(stoul(leaf_cls)) == true);
    }

    ExpectSuccess(api.Stop(name));
    Expect(TaskRunning(api, pid) == false);

    if (NetworkEnabled()) {
        Expect(TcClassExist(stoul(leaf_cls)) == false);

        Say() << "Check that destroying container removes tclass" << std::endl;
        ExpectSuccess(api.Start(name));
        Expect(TcClassExist(stoul(root_cls)) == true);
        Expect(TcClassExist(stoul(leaf_cls)) == true);
        ExpectSuccess(api.Destroy(name));
        Expect(TaskRunning(api, pid) == false);
        Expect(TcClassExist(stoul(leaf_cls)) == false);
        ExpectSuccess(api.Create(name));
    }

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

static void TestIsolateProperty(TPortoAPI &api) {
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

    if (NetworkEnabled()) {
        Say() << "Make sure container has correct network class" << std::endl;

        string handle = GetCgKnob("net_cls", name, "net_cls.classid");
        Expect(handle != "0");

        Expect(TcClassExist(stoul(handle)) == true);
        ExpectSuccess(api.Stop(name));
        Expect(TcClassExist(stoul(handle)) == false);
    }
    ExpectSuccess(api.Destroy(name));

    Say() << "Make sure isolate works correctly with meta parent" << std::endl;
    string pid;

    ExpectSuccess(api.Create("meta"));
    ExpectSuccess(api.Create("meta/test"));

    ExpectSuccess(api.SetProperty("meta/test", "isolate", "false"));
    ExpectSuccess(api.SetProperty("meta/test", "command", "sleep 1000"));
    ExpectSuccess(api.Start("meta/test"));
    ExpectSuccess(api.GetData("meta/test", "root_pid", pid));
    AsRoot(api);
    Expect(GetNamespace("self", "pid") == GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectSuccess(api.Stop("meta/test"));

    ExpectSuccess(api.SetProperty("meta/test", "isolate", "true"));
    ExpectSuccess(api.SetProperty("meta/test", "command", "ps aux"));
    ExpectSuccess(api.Start("meta/test"));
    ExpectSuccess(api.GetData("meta/test", "root_pid", pid));
    AsRoot(api);
    Expect(GetNamespace("self", "pid") != GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectSuccess(api.Stop("meta/test"));

    ExpectSuccess(api.Destroy("meta/test"));
    ExpectSuccess(api.Destroy("meta"));

    ExpectSuccess(api.Create("test"));
    ExpectSuccess(api.Create("test/meta"));
    ExpectSuccess(api.Create("test/meta/test"));

    ExpectSuccess(api.SetProperty("test", "command", "sleep 1000"));
    ExpectSuccess(api.Start("test"));

    ExpectSuccess(api.SetProperty("test/meta/test", "command", "sleep 1000"));
    ExpectSuccess(api.Start("test/meta/test"));
    ExpectSuccess(api.GetData("test", "root_pid", pid));
    ExpectSuccess(api.GetData("test/meta/test", "root_pid", ret));
    AsRoot(api);
    Expect(GetNamespace(ret, "pid") != GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectSuccess(api.Stop("test/meta/test"));

    ExpectSuccess(api.SetProperty("test/meta/test", "isolate", "false"));
    ExpectSuccess(api.Start("test/meta/test"));
    ExpectSuccess(api.GetData("test", "root_pid", pid));
    ExpectSuccess(api.GetData("test/meta/test", "root_pid", ret));
    AsRoot(api);
    Expect(GetNamespace(ret, "pid") == GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectSuccess(api.Stop("test/meta/test"));

    ExpectSuccess(api.Destroy("test/meta/test"));
    ExpectSuccess(api.Destroy("test/meta"));
    ExpectSuccess(api.Destroy("test"));
}

static void TestEnvTrim(TPortoAPI &api) {
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

static void TestEnvProperty(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));

    AsRoot(api);

    Say() << "Check default environment" << std::endl;

    static const char empty_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "container=lxc\0"
        "PORTO_NAME=a\0"
        "PORTO_HOST=" HOSTNAME "\0"
        "HOME=/place/porto/a\0"
        "USER=nobody\0";
    ExpectEnv(api, name, "", empty_env, sizeof(empty_env));

    Say() << "Check user-defined environment" << std::endl;
    static const char ab_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "a=b\0"
        "c=d\0"
        "container=lxc\0"
        "PORTO_NAME=a\0"
        "PORTO_HOST=" HOSTNAME "\0"
        "HOME=/place/porto/a\0"
        "USER=nobody\0";

    ExpectEnv(api, name, "a=b;c=d;", ab_env, sizeof(ab_env));
    ExpectEnv(api, name, "a=b;;c=d;", ab_env, sizeof(ab_env));

    static const char asb_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "a=e;b\0"
        "c=d\0"
        "container=lxc\0"
        "PORTO_NAME=a\0"
        "PORTO_HOST=" HOSTNAME "\0"
        "HOME=/place/porto/a\0"
        "USER=nobody\0";
    ExpectEnv(api, name, "a=e\\;b;c=d;", asb_env, sizeof(asb_env));

    ExpectSuccess(api.SetProperty(name, "command", "sleep $N"));
    ExpectSuccess(api.SetProperty(name, "env", "N=1"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestUserGroupProperty(TPortoAPI &api) {
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

    ExpectFailure(api.Start(name), EError::Permission);

    AsRoot(api);
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("daemon"));
    Expect(gid == GroupGid("bin"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
    AsNobody(api);
}

static void TestCwdProperty(TPortoAPI &api) {
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

static void TestStdPathProperty(TPortoAPI &api) {
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

static void TestRootRdOnlyProperty(TPortoAPI &api) {
    string name = "a";
    TPath path(TMPDIR + "/" + name);
    string ROnly;
    string ret;

    RemakeDir(api, path);

    Say() << "Check root read only property" << std::endl;
    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.GetProperty(name, "root_readonly", ROnly));
    Expect(ROnly == "false");

    ExpectSuccess(api.SetProperty(name, "root", path.ToString()));
    AsRoot(api);
    BootstrapCommand("/usr/bin/touch", path.ToString());
    path.Chown("nobody", "nogroup");
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "/touch test"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("0"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "root_readonly", "true"));
    ExpectSuccess(api.SetProperty(name, "command", "/touch test2"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret != string("0"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
};

static void TestRootProperty(TPortoAPI &api) {
    string pid;
    string v;

    string name = "a";
    string path = TMPDIR + "/" + name;

    Say() << "Make sure root is empty" << std::endl;

    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.SetProperty(name, "command", "ls"));
    ExpectSuccess(api.SetProperty(name, "root", path));

    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectSuccess(api.GetData(name, "start_errno", v));
    Expect(v == string("2"));

    ExpectSuccess(api.Destroy(name));

    Say() << "Check filesystem isolation" << std::endl;

    ExpectSuccess(api.Create(name));

    RemakeDir(api, path);

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

    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v == string("/\n"));
    ExpectSuccess(api.Stop(name));

    Say() << "Check /dev layout" << std::endl;

    ExpectSuccess(api.SetProperty(name, "command", "/ls -1 /dev"));
    v = StartWaitAndGetData(api, name, "stdout");

    vector<string> devs = { "null", "zero", "full", "urandom", "random" };
    vector<string> other = { "ptmx", "pts", "shm", "fd" };
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

    cwd = config().container().tmp_dir() + "/" + name;

    TFolder f(cwd);
    AsRoot(api);
    if (f.Exists()) {
        error = f.Remove(true);
        if (error)
            throw error.GetMsg();
    }
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
    string path = TMPDIR + "/" + name;

    RemakeDir(api, path);

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
    AsRoot(api);
    BootstrapCommand("/bin/cat", path);
    AsNobody(api);

    TFolder d(path + "/etc");
    TFile f(path + "/etc/hostname");
    AsRoot(api);
    if (!d.Exists())
        ExpectSuccess(d.Create());
    ExpectSuccess(f.Touch());
    ExpectSuccess(f.GetPath().Chown(GetDefaultUser(), GetDefaultGroup()));
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "/cat /etc/hostname"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", v));
    Expect(v != GetHostname() + "\n");
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
    auto m = ParseMountinfo(v);

    Expect(m[path + "/bin"] == "ro,relatime");
    Expect(m[path + "/tmp"] == "rw,relatime" || m["/tmp"] == "rw");
    ExpectSuccess(api.Stop(name));

    path = TMPDIR + "/" + name;
    RemakeDir(api, path);

    AsRoot(api);
    BootstrapCommand("/bin/cat", path);
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    ExpectSuccess(api.SetProperty(name, "root", path));
    ExpectSuccess(api.SetProperty(name, "bind", "/bin /bin ro; /tmp/27389 /tmp"));
    v = StartWaitAndGetData(api, name, "stdout");
    m = ParseMountinfo(v);
    Expect(m["/"] == "rw,relatime");
    Expect(m["/bin"] == "ro,relatime");
    Expect(m["/tmp"] == "rw,relatime" || m["/tmp"] == "rw");
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static vector<string> StringToVec(const std::string &s) {
    vector<string> lines;

    TError error = SplitString(s, '\n', lines);
    if (error)
        throw error.GetMsg();
    return lines;
}

struct LinkInfo {
    std::string hw;
    std::string master;
    std::string mtu;
    bool up;
};

static map<string, LinkInfo> IfHw(const vector<string> &iplines) {
    map<string, LinkInfo> ret;
    for (auto &ipline : iplines) {
        vector<string> lines;
        TError error = SplitString(ipline, '\\', lines);
        if (error)
            throw error.GetMsg();
        if (lines.size() < 2)
            throw "Invalid interface: " + ipline;

        vector<string> tokens;
        error = SplitString(lines[0], ':', tokens);
        if (error)
            throw error.GetMsg();
        if (tokens.size() < 2)
            throw "Invalid line 1: " + lines[0];

        string fulliface = StringTrim(tokens[1]);
        string flags = StringTrim(tokens[2]);

        bool up = flags.find("DOWN") == std::string::npos;
        string master = "";
        string mtu = "";

        auto pos = flags.find("master");
        if (pos != std::string::npos) {
            auto begin = pos + strlen("master ");
            auto end = flags.find(" ", begin);
            master = string(flags, begin, end - begin);
        }

        pos = ipline.find("mtu");
        if (pos != std::string::npos) {
            auto begin = pos + strlen("mtu ");
            auto end = ipline.find(" ", begin);
            mtu = string(ipline, begin, end - begin);
        }

        tokens.clear();
        error = SplitString(fulliface, '@', tokens);
        if (error)
            throw error.GetMsg();

        string iface = StringTrim(tokens[0]);

        tokens.clear();
        error = SplitString(StringTrim(lines[1]), ' ', tokens);
        if (error)
            throw error.GetMsg();
        if (tokens.size() < 2)
            throw "Invalid line 2: " + lines[1];

        string hw = StringTrim(tokens[1]);

        struct LinkInfo li = { hw, master, mtu, up };
        ret[iface] = li;
    }

    return ret;
}

static bool ShareMacAddress(const vector<string> &a, const vector<string> &b) {
    auto ahw = IfHw(a);
    auto bhw = IfHw(b);

    for (auto apair : ahw) {
        if (apair.second.hw == "00:00:00:00:00:00")
            continue;

        for (auto bpair : bhw) {
            if (apair.second.hw == bpair.second.hw)
                return true;
        }
    }

    return false;
}

static string System(const std::string &cmd) {
    vector<string> lines = Popen(cmd);
    Expect(lines.size() == 1);
    return StringTrim(lines[0]);
}

static void TestNetProperty(TPortoAPI &api) {
    if (!NetworkEnabled()) {
        Say() << "Make sure network namespace is shared when network disabled" << std::endl;

        string pid;

        string name = "a";
        ExpectSuccess(api.Create(name));

        Say() << "Spawn long running task" << std::endl;
        ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectSuccess(api.Start(name));
        ExpectSuccess(api.GetData(name, "root_pid", pid));
        Expect(TaskRunning(api, pid) == true);

        AsRoot(api);
        Expect(GetNamespace("self", "net") == GetNamespace(pid, "net"));

        ExpectSuccess(api.Destroy(name));

        return;
    }

    string name = "a";
    ExpectSuccess(api.Create(name));

    vector<string> hostLink = Popen("ip -o -d link show");

    string link = links[0]->GetAlias();

    Say() << "Check net parsing" << std::endl;
    ExpectFailure(api.SetProperty(name, "net", "qwerty"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", ""), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "net", "host"));
    ExpectSuccess(api.SetProperty(name, "net", "none"));
    ExpectFailure(api.SetProperty(name, "net", "host; macvlan " + link + " " + link), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "host; host veth0"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "host; host"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "host; none"), EError::InvalidValue);

    Say() << "Check net=none" << std::endl;

    ExpectSuccess(api.SetProperty(name, "net", "none"));
    ExpectSuccess(api.SetProperty(name, "command", "ip -o -d link show"));
    string s = StartWaitAndGetData(api, name, "stdout");
    auto containerLink = StringToVec(s);
    Expect(containerLink.size() == 1);
    Expect(containerLink.size() != hostLink.size());
    Expect(ShareMacAddress(hostLink, containerLink) == false);
    auto linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    Expect(linkMap.at("lo").up == true);
    ExpectSuccess(api.Stop(name));

    Say() << "Check net=host" << std::endl;
    ExpectSuccess(api.SetProperty(name, "net", "host"));
    s = StartWaitAndGetData(api, name, "stdout");
    containerLink = StringToVec(s);
    Expect(containerLink.size() == hostLink.size());
    Expect(ShareMacAddress(hostLink, containerLink) == true);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    Expect(linkMap.at("lo").up == true);
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "command", "ip -o -d link show"));

    Say() << "Check net=host:veth0" << std::endl;

    AsRoot(api);
    (void)system("ip link delete veth0");
    Expect(system("ip link add veth0 type veth peer name veth1") == 0);
    AsNobody(api);

    ExpectFailure(api.SetProperty(name, "net", "host invalid"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "net", "host veth0"));
    s = StartWaitAndGetData(api, name, "stdout");
    containerLink = StringToVec(s);
    Expect(containerLink.size() == 2);
    Expect(containerLink.size() != hostLink.size());
    Expect(ShareMacAddress(hostLink, containerLink) == false);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    Expect(linkMap.at("lo").up == true);
    Expect(linkMap.find("veth0") != linkMap.end());
    ExpectSuccess(api.Stop(name));

    Say() << "Make sure net=host:veth0 doesn't preserve L3 address" << std::endl;
    AsRoot(api);
    Expect(system("ip link add veth0 type veth peer name veth1") == 0);
    Expect(system("ip addr add dev veth0 1.2.3.4") == 0);
    AsNobody(api);

    ExpectSuccess(api.SetProperty(name, "command", "ip -o -d addr show dev veth0 to 1.2.3.4"));
    ExpectSuccess(api.SetProperty(name, "net", "host veth0"));
    s = StartWaitAndGetData(api, name, "stdout");
    Expect(s == "");
    ExpectSuccess(api.Stop(name));

    Say() << "Check net=macvlan" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "ip -o -d link show"));
    ExpectFailure(api.SetProperty(name, "net", "macvlan"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "macvlan invalid " + link), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "net", "macvlan " + link), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "net", "macvlan " + link + " " + link));
    s = StartWaitAndGetData(api, name, "stdout");
    containerLink = StringToVec(s);
    Expect(containerLink.size() == 2);
    Expect(containerLink.size() != hostLink.size());
    Expect(ShareMacAddress(hostLink, containerLink) == false);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    Expect(linkMap.at("lo").up == true);
    Expect(linkMap.find(link) != linkMap.end());
    Expect(linkMap.at(link).up == true);
    ExpectSuccess(api.Stop(name));

    string mtu = "1400";
    ExpectSuccess(api.SetProperty(name, "net", "macvlan " + link + " eth10 bridge " + mtu));
    s = StartWaitAndGetData(api, name, "stdout");
    containerLink = StringToVec(s);
    Expect(containerLink.size() == 2);
    Expect(containerLink.size() != hostLink.size());
    Expect(ShareMacAddress(hostLink, containerLink) == false);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    Expect(linkMap.at("lo").up == true);
    Expect(linkMap.find("eth10") != linkMap.end());
    Expect(linkMap.at("eth10").mtu == mtu);
    Expect(linkMap.at("eth10").up == true);
    ExpectSuccess(api.Stop(name));

    string hw = "00:11:22:33:44:55";
    ExpectSuccess(api.SetProperty(name, "net", "macvlan " + link + " eth10 bridge -1 " + hw));
    s = StartWaitAndGetData(api, name, "stdout");
    containerLink = StringToVec(s);
    Expect(containerLink.size() == 2);
    Expect(containerLink.size() != hostLink.size());
    Expect(ShareMacAddress(hostLink, containerLink) == false);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    Expect(linkMap.at("lo").up == true);
    Expect(linkMap.find("eth10") != linkMap.end());
    Expect(linkMap.at("eth10").hw == hw);
    Expect(linkMap.at("eth10").up == true);
    ExpectSuccess(api.Stop(name));

    Say() << "Check net=macvlan statistics" << std::endl;
    // create macvlan on default interface and ping ya.ru
    string uniq = "123";
    string gw = System("ip -o route | grep default | cut -d' ' -f3");
    string dev = System("ip -o route get " + gw + " | awk '{print $3}'");
    string addr = System("ip -o addr show " + dev + " | grep -w inet | awk '{print $4}'");
    string ip = System("echo " + addr + " | sed -e 's@\\([0-9]*\\.[0-9]*\\.[0-9]*\\.\\)[0-9]*\\(.*\\)@\\1" + uniq + "\\2@'");

    Say() << "Using device " << dev << " gateway " << gw << " ip " << addr << " -> " << ip << std::endl;
    ExpectSuccess(api.SetProperty(name, "net", "macvlan " + dev + " " + dev));
    ExpectSuccess(api.SetProperty(name, "command", "false"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "net_bytes[" + dev + "]", s));
    Expect(s == "0");

    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'ip addr add " + ip + " dev " + dev + " && ip route add default via " + gw + " && ping ya.ru -c 1 -w 1'"));
    AsRoot(api);
    ExpectSuccess(api.SetProperty(name, "user", "root"));
    ExpectSuccess(api.SetProperty(name, "group", "root"));

    ExpectSuccess(api.Start(name));
    AsNobody(api);
    WaitState(api, name, "dead", 60);
    ExpectSuccess(api.GetData(name, "net_bytes[" + dev + "]", s));
    Expect(s != "0");

    Say() << "Check net=veth" << std::endl;
    AsRoot(api);
    ExpectSuccess(api.Destroy(name));
    (void)system("ip link delete portobr0");
    Expect(system("ip link add portobr0 type bridge") == 0);
    Expect(system("ip link set portobr0 up") == 0);
    AsNobody(api);

    ExpectSuccess(api.Create(name));
    ExpectFailure(api.SetProperty(name, "net", "veth eth0 invalid"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "net", "veth eth0 portobr0"));
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'sleep 1 && ip -o -d link show'"));

    auto pre = IfHw(Popen("ip -o -d link show"));
    ExpectSuccess(api.Start(name));
    auto post = IfHw(Popen("ip -o -d link show"));
    Expect(pre.size() + 1 == post.size());
    for (auto kv : pre)
        post.erase(kv.first);
    Expect(post.size() == 1);
    auto portove = post.begin()->first;
    Expect(post[portove].master == "portobr0");

    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "stdout", s));
    containerLink = StringToVec(s);
    Expect(containerLink.size() == 2);
    Expect(containerLink.size() != hostLink.size());
    Expect(ShareMacAddress(hostLink, containerLink) == false);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    Expect(linkMap.at("lo").up == true);
    Expect(linkMap.find("eth0") != linkMap.end());
    ExpectSuccess(api.Stop(name));

    post = IfHw(Popen("ip -o -d link show"));
    Expect(post.find(portove) == post.end());

    AsRoot(api);
    ExpectSuccess(api.Destroy(name));
}

static void TestAllowedDevicesProperty(TPortoAPI &api) {
    string name = "a";
    ExpectSuccess(api.Create(name));

    Say() << "Checking default allowed_devices" << std::endl;

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    Expect(GetCgKnob("devices", name, "devices.list") == "a *:* rwm");
    ExpectSuccess(api.Stop(name));

    Say() << "Checking custom allowed_devices" << std::endl;

    ExpectSuccess(api.SetProperty(name, "allowed_devices", "c 1:3 rwm; c 1:5 rwm"));
    ExpectSuccess(api.Start(name));
    Expect(GetCgKnob("devices", name, "devices.list") == "c 1:3 rwm\nc 1:5 rwm");
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestCapabilitiesProperty(TPortoAPI &api) {
    string pid;
    string name = "a";

    int lastCap;
    TFile f("/proc/sys/kernel/cap_last_cap");
    TError error = f.AsInt(lastCap);
    if (error)
        throw error.GetMsg();

    uint64_t defaultCap = 0;
    for (int i = 0; i <= lastCap; i++)
        defaultCap |= (1ULL << i);

    uint64_t customCap = (1ULL << CAP_CHOWN) |
        (1ULL << CAP_DAC_OVERRIDE) |
        (1ULL << CAP_FSETID) |
        (1ULL << CAP_FOWNER) |
        (1ULL << CAP_MKNOD) |
        (1ULL << CAP_NET_RAW) |
        (1ULL << CAP_SETGID) |
        (1ULL << CAP_SETUID) |
        (1ULL << CAP_SETFCAP) |
        (1ULL << CAP_SETPCAP) |
        (1ULL << CAP_NET_BIND_SERVICE) |
        (1ULL << CAP_SYS_CHROOT) |
        (1ULL << CAP_KILL) |
        (1ULL << CAP_AUDIT_WRITE);

    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));

    Say() << "Make sure capabilities don't work for non-root container" << std::endl;

    ExpectFailure(api.SetProperty(name, "capabilities", "CHOWN"), EError::Permission);

    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(GetCap(pid, "CapInh") == 0);
    Expect(GetCap(pid, "CapPrm") == 0);
    Expect(GetCap(pid, "CapEff") == 0);
    Expect(GetCap(pid, "CapBnd") == defaultCap);
    ExpectSuccess(api.Stop(name));


    AsRoot(api);
    ExpectSuccess(api.SetProperty(name, "user", "root"));
    ExpectSuccess(api.SetProperty(name, "group", "root"));

    Say() << "Checking default capabilities" << std::endl;
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    Expect(GetCap(pid, "CapInh") == defaultCap);
    Expect(GetCap(pid, "CapPrm") == defaultCap);
    Expect(GetCap(pid, "CapEff") == defaultCap);
    Expect(GetCap(pid, "CapBnd") == defaultCap);

    ExpectSuccess(api.Stop(name));

    Say() << "Checking custom capabilities" << std::endl;
    ExpectFailure(api.SetProperty(name, "capabilities", "CHOWN; INVALID"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "capabilities", "CHOWN; DAC_OVERRIDE; FSETID; FOWNER; MKNOD; NET_RAW; SETGID; SETUID; SETFCAP; SETPCAP; NET_BIND_SERVICE; SYS_CHROOT; KILL; AUDIT_WRITE"));

    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    Expect(GetCap(pid, "CapInh") == customCap);
    Expect(GetCap(pid, "CapPrm") == customCap);
    Expect(GetCap(pid, "CapEff") == customCap);
    Expect(GetCap(pid, "CapBnd") == customCap);

    ExpectSuccess(api.Stop(name));

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
        "allowed_devices",
        "root_readonly",
        "capabilities",
    };

    vector<string> data = {
        "state",
        "oom_killed",
        "parent",
        "respawn_count",
        "root_pid",
        "exit_status",
        "start_errno",
        "stdout",
        "stderr",
        "cpu_usage",
        "memory_usage",
        "net_bytes",
        "net_packets",
        "net_drops",
        "net_overlimits",
        "minor_faults",
        "major_faults",
        "io_read",
        "io_write",
        "time",
    };

    std::vector<TProperty> plist;

    ExpectSuccess(api.Plist(plist));
    Expect(plist.size() == properties.size());

    for (auto p: plist)
        Expect(std::find(properties.begin(), properties.end(), p.Name) != properties.end());

    std::vector<TData> dlist;

    ExpectSuccess(api.Dlist(dlist));
    Expect(dlist.size() == data.size());

    for (auto d: dlist)
        Expect(std::find(data.begin(), data.end(), d.Name) != data.end());

    Say() << "Check root cpu_usage & memory_usage" << std::endl;
    ExpectSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "memory_usage", v));
    Expect(v == "0");

    for (auto &link : links) {
        ExpectSuccess(api.GetData(root, "net_bytes[" + link->GetAlias() + "]", v));
        Expect(v == "0");
        ExpectSuccess(api.GetData(root, "net_packets[" + link->GetAlias() + "]", v));
        Expect(v == "0");
        ExpectSuccess(api.GetData(root, "net_drops[" + link->GetAlias() + "]", v));
        Expect(v == "0");
        ExpectSuccess(api.GetData(root, "net_overlimits[" + link->GetAlias() + "]", v));
        Expect(v == "0");
    }

    if (IsCfqActive()) {
        ExpectSuccess(api.GetData(root, "io_read", v));
        Expect(v == "");
        ExpectSuccess(api.GetData(root, "io_write", v));
        Expect(v == "");
    }

    if (NetworkEnabled()) {
        uint32_t defClass = TcHandle(1, 2);
        uint32_t rootClass = TcHandle(1, 1);
        uint32_t nextClass = TcHandle(1, 3);

        uint32_t rootQdisc = TcHandle(1, 0);
        uint32_t nextQdisc = TcHandle(2, 0);

        Expect(TcQdiscExist(rootQdisc) == true);
        Expect(TcQdiscExist(nextQdisc) == false);
        Expect(TcClassExist(defClass) == true);
        Expect(TcClassExist(rootClass) == true);
        Expect(TcClassExist(nextClass) == false);
        Expect(TcCgFilterExist(rootQdisc, 1) == true);
        Expect(TcCgFilterExist(rootQdisc, 2) == false);
    }

    Say() << "Check root properties & data" << std::endl;
    for (auto p : properties)
        ExpectFailure(api.GetProperty(root, p, v), EError::InvalidProperty);

    ExpectSuccess(api.GetData(root, "state", v));
    Expect(v == string("meta"));
    ExpectFailure(api.GetData(root, "exit_status", v), EError::InvalidState);
    ExpectFailure(api.GetData(root, "start_errno", v), EError::InvalidState);
    ExpectFailure(api.GetData(root, "root_pid", v), EError::InvalidState);
    ExpectFailure(api.GetData(root, "stdout", v), EError::InvalidState);
    ExpectSuccess(api.GetData(root, "parent", v));
    Expect(v == "");
    ExpectFailure(api.GetData(root, "stderr", v), EError::InvalidState);

    Say() << "Check that stop on root stops all children" << std::endl;

    ExpectSuccess(api.Create("a"));
    ExpectSuccess(api.Create("b"));
    ExpectSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty("b", "command", "sleep 1000"));
    ExpectSuccess(api.Start("a"));
    ExpectSuccess(api.Start("b"));

    ExpectSuccess(api.Stop(root));

    ExpectSuccess(api.GetData("a", "state", v));
    Expect(v == "stopped");
    ExpectSuccess(api.GetData("b", "state", v));
    Expect(v == "stopped");
    ExpectSuccess(api.GetData(root, "state", v));
    Expect(v == "meta");

    ExpectFailure(api.Destroy(root), EError::InvalidValue);
    ExpectSuccess(api.Destroy("a"));
    ExpectSuccess(api.Destroy("b"));
}

static void TestDataMap(TPortoAPI &api, const std::string &name, const std::string &data) {
    std::string full;
    vector<string> lines;

    ExpectSuccess(api.GetData(name, data, full));
    Expect(full != "");
    ExpectSuccess(SplitString(full, ';', lines));

    Expect(lines.size() != 0);
    for (auto line: lines) {
        string tmp;
        vector<string> tokens;

        ExpectSuccess(SplitString(full, ':', tokens));
        ExpectSuccess(api.GetData(name, data + "[" + StringTrim(tokens[0]) + "]", tmp));
        Expect(tmp == StringTrim(tokens[1]));
    }

    ExpectFailure(api.GetData(name, data + "[invalid]", full), EError::InvalidValue);
}

static void ExpectNonZeroLink(TPortoAPI &api, const std::string &name,
                              const std::string &data) {
#if 0
    int nonzero = 0;

    for (auto &link : links) {
        string v;
        ExpectSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));

        if (v != "0" && v != "-1")
            nonzero++;
    }
    Expect(nonzero != 0);
#else
    for (auto &link : links) {
        string v;
        ExpectSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));
        Expect(v != "0" && v != "-1");
    }
#endif
}

static void ExpectRootLink(TPortoAPI &api, const std::string &name,
                           const std::string &data) {
    for (auto &link : links) {
        string v, rv;
        ExpectSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));
        ExpectSuccess(api.GetData("/", data + "[" + link->GetAlias() + "]", rv));
        Expect(v == rv);
    }
}

static void ExpectZeroLink(TPortoAPI &api, const std::string &name,
                           const std::string &data) {
    for (auto &link : links) {
        string v;
        ExpectSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));
        Expect(v == "0");
    }
}

static void TestData(TPortoAPI &api) {
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
    if (NetworkEnabled())
        ExpectSuccess(api.SetProperty(wget, "command", "bash -c 'wget yandex.ru && sync'"));
    else
        ExpectSuccess(api.SetProperty(wget, "command", "bash -c 'dd if=/dev/urandom bs=4M count=1 of=/tmp/porto.tmp && sync'"));
    ExpectSuccess(api.Start(wget));
    WaitState(api, wget, "dead");

    string v, rv;
    ExpectSuccess(api.GetData(wget, "exit_status", v));
    Expect(v == string("0"));
    ExpectSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(root, "memory_usage", v));
    Expect(v != "0" && v != "-1");

    if (IsCfqActive()) {
        ExpectSuccess(api.GetData(root, "io_write", v));
        Expect(v != "");
        TestDataMap(api, root, "io_write");
        ExpectSuccess(api.GetData(root, "io_read", v));
        Expect(v != "");
        TestDataMap(api, root, "io_read");
    }
    ExpectSuccess(api.GetData(wget, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(wget, "memory_usage", v));
    Expect(v != "0" && v != "-1");
    if (IsCfqActive()) {
        ExpectSuccess(api.GetData(wget, "io_write", v));
        Expect(v != "");
        ExpectSuccess(api.GetData(wget, "io_read", v));
        Expect(v != "");
    }

    ExpectSuccess(api.GetData(noop, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectSuccess(api.GetData(noop, "memory_usage", v));
    Expect(v != "0" && v != "-1");
    if (IsCfqActive()) {
        ExpectSuccess(api.GetData(noop, "io_write", v));
        Expect(v == "");
        ExpectSuccess(api.GetData(noop, "io_read", v));
        Expect(v == "");
    }

    if (NetworkEnabled()) {
        ExpectNonZeroLink(api, root, "net_bytes");
        ExpectRootLink(api, wget, "net_bytes");
        ExpectZeroLink(api, noop, "net_bytes");

        ExpectNonZeroLink(api, root, "net_packets");
        ExpectRootLink(api, wget, "net_packets");
        ExpectZeroLink(api, noop, "net_packets");

        ExpectZeroLink(api, root, "net_drops");
        ExpectZeroLink(api, wget, "net_drops");
        ExpectZeroLink(api, noop, "net_drops");

        ExpectZeroLink(api, root, "net_overlimits");
        ExpectZeroLink(api, wget, "net_overlimits");
        ExpectZeroLink(api, noop, "net_overlimits");
    }

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

    uint32_t i = 0;
    for (auto &link : links) {
        ExpectSuccess(api.SetProperty(name, "net_guarantee[" + link->GetAlias() + "]", std::to_string(netGuarantee + i)));
        ExpectSuccess(api.SetProperty(name, "net_ceil[" + link->GetAlias() + "]", std::to_string(netCeil + i)));
        ExpectFailure(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", "-1"), EError::InvalidValue);
        ExpectFailure(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", "8"), EError::InvalidValue);
        ExpectSuccess(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", "0"));
        ExpectSuccess(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", std::to_string(netPrio + i)));
        i++;
    }
    ExpectSuccess(api.Start(name));

    if (NetworkEnabled()) {
        string handle = GetCgKnob("net_cls", name, "net_cls.classid");

        i = 0;
        for (auto &link : links) {
            uint32_t prio, rate, ceil;
            TNlClass tclass(link, -1, stoul(handle));
            ExpectSuccess(tclass.GetProperties(prio, rate, ceil));
            Expect(prio == netPrio + i);
            Expect(rate == netGuarantee + i);
            Expect(ceil == netCeil + i);

            i++;
        }
    }

    ExpectSuccess(api.Destroy(name));
}

static void TestUlimitProperty(TPortoAPI &api) {
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

    string v;
    ExpectSuccess(api.GetData(parent, "state", v));
    Expect(v == "running");
    ExpectSuccess(api.GetData(child, "state", v));
    Expect(v == "running");

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
           ExpectSuccess(api.GetProperty(name, "command", s));

    AsUser(api, daemonUser, daemonGroup);
        ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectFailure(api.SetProperty(name, "user", "mail"), EError::Permission);
        ExpectFailure(api.SetProperty(name, "group", "mail"), EError::Permission);
        ExpectSuccess(api.GetProperty(name, "command", s));
        ExpectSuccess(api.Start(name));
        ExpectSuccess(api.GetData(name, "root_pid", s));

    AsUser(api, binUser, binGroup);
        ExpectSuccess(api.GetData(name, "root_pid", s));
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

static void WaitRespawn(TPortoAPI &api, const std::string &name, int expected, int maxTries = 10) {
    std::string respawnCount;
    int successRespawns = 0;
    for(int i = 0; i < maxTries; i++) {
        sleep(config().container().respawn_delay_ms() / 1000);
        ExpectSuccess(api.GetData(name, "respawn_count", respawnCount));
        if (respawnCount == std::to_string(expected))
            successRespawns++;
        if (successRespawns == 2)
            break;
        Say() << "Respawned " << i << " times" << std::endl;
    }
    Expect(std::to_string(expected) == respawnCount);
}

static void TestRespawnProperty(TPortoAPI &api) {
    string pid, respawnPid;
    string ret;

    string name = "a";
    ExpectSuccess(api.Create(name));
    ExpectFailure(api.SetProperty(name, "max_respawns", "true"), EError::InvalidValue);

    ExpectSuccess(api.SetProperty(name, "command", "true"));

    ExpectSuccess(api.SetProperty(name, "respawn", "false"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "respawn_count", ret));
    Expect(ret == string("0"));
    WaitState(api, name, "dead");
    sleep(config().container().respawn_delay_ms() / 1000);
    ExpectSuccess(api.GetData(name, "respawn_count", ret));
    Expect(ret == string("0"));
    ExpectSuccess(api.Stop(name));

    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitState(api, name, "dead");
    WaitState(api, name, "running");
    ExpectSuccess(api.GetData(name, "root_pid", respawnPid));
    Expect(pid != respawnPid);
    ExpectSuccess(api.GetData(name, "respawn_count", ret));
    Expect(ret != "0" && ret != "");
    ExpectSuccess(api.Stop(name));

    int expected = 3;
    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.SetProperty(name, "max_respawns", std::to_string(expected)));
    ExpectSuccess(api.SetProperty(name, "command", "echo test"));
    ExpectSuccess(api.Start(name));

    WaitRespawn(api, name, expected);

    ExpectSuccess(api.Destroy(name));
}

static void TestLeaks(TPortoAPI &api) {
    string slavePid, masterPid;
    string name;
    int slack = 4096;

    TFile slaveFile(config().slave_pid().path());
    ExpectSuccess(slaveFile.AsString(slavePid));
    TFile masterFile(config().master_pid().path());
    ExpectSuccess(masterFile.AsString(masterPid));

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

    int prevSlave = GetVmRss(slavePid);
    int prevMaster = GetVmRss(masterPid);

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

    int nowSlave = GetVmRss(slavePid);
    int nowMaster = GetVmRss(masterPid);

    Say() << "Expected slave " << nowSlave << " < " << prevSlave + slack << std::endl;
    Expect(nowSlave <= prevSlave + slack);

    Say() << "Expected master " << nowMaster << " < " << prevMaster + slack << std::endl;
    Expect(nowMaster <= prevMaster + slack);
}

static void TestPerf(TPortoAPI &api) {
    std::string name, v;
    size_t begin, ms;
    const int nr = 1000;

    begin = GetCurrentTimeMs();
    for (int i = 0; i < nr; i++) {
        name = "perf" + std::to_string(i);
        ExpectSuccess(api.Create(name));
        ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectSuccess(api.Start(name));
    }
    ms = GetCurrentTimeMs() - begin;
    Say() << "Create " << nr << " containers took " << ms / 1000.0 << "s" << std::endl;
    Expect(ms < 10 * 1000);

    begin = GetCurrentTimeMs();
    for (int i = 0; i < nr; i++) {
        name = "perf" + std::to_string(i);
        ExpectSuccess(api.GetData(name, "state", v));
    }
    ms = GetCurrentTimeMs() - begin;
    Say() << "Get state " << nr << " containers took " << ms / 1000.0 << "s" << std::endl;
    Expect(ms < 100);

    begin = GetCurrentTimeMs();
    for (int i = 0; i < nr; i++) {
        name = "perf" + std::to_string(i);
        ExpectSuccess(api.Destroy(name));
    }
    ms = GetCurrentTimeMs() - begin;
    Say() << "Destroy " << nr << " containers took " << ms / 1000.0 << "s" << std::endl;
    Expect(ms < 60 * 1000);
}

static void KillPorto(TPortoAPI &api, int sig, int times = 10) {
    int portodPid = ReadPid(config().slave_pid().path());
    if (kill(portodPid, sig))
        throw "Can't send " + std::to_string(sig) + " to slave";
    WaitExit(api, std::to_string(portodPid));
    WaitPortod(api, times);
    expectedRespawns++;

    std::string v;
    ExpectSuccess(api.GetData("/", "porto_stat[spawned]", v));
    Expect(v == std::to_string(expectedRespawns + 1));
}

static bool RespawnTicks(TPortoAPI &api, const std::string &name, int maxTries = 3) {
    std::string respawnCount, v;
    ExpectSuccess(api.GetData(name, "respawn_count", respawnCount));
    for(int i = 0; i < maxTries; i++) {
        sleep(config().container().respawn_delay_ms() / 1000);
        ExpectSuccess(api.GetData(name, "respawn_count", v));

        if (v != respawnCount)
            return true;
    }
    return false;
}

static void TestRecovery(TPortoAPI &api) {
    string pid, v;
    string name = "a:b";

    map<string,string> props = {
        { "command", "sleep 1000" },
        { "user", "bin" },
        { "group", "daemon" },
        { "env", "a=a; b=b" },
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

    KillPorto(api, SIGKILL);

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
    ExpectSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectSuccess(api.Start(child));

    AsRoot(api);
    KillPorto(api, SIGKILL);
    AsNobody(api);

    std::vector<std::string> containers;

    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 3);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("a/b"));
    ExpectSuccess(api.GetData(parent, "state", v));
    Expect(v == "meta");

    ExpectSuccess(api.SetProperty(parent, "recharge_on_pgfault", "true"));
    ExpectFailure(api.SetProperty(parent, "env", "a=b"), EError::InvalidState);

    ExpectSuccess(api.GetData(child, "state", v));
    Expect(v == "running");
    ExpectSuccess(api.Destroy(child));
    ExpectSuccess(api.Destroy(parent));

    Say() << "Make sure task is moved to correct cgroup on recovery" << std::endl;
    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    ExpectSuccess(api.GetData(name, "root_pid", pid));

    AsRoot(api);
    TFile f("/sys/fs/cgroup/memory/porto/cgroup.procs");
    ExpectSuccess(f.AppendString(pid));
    auto cgmap = GetCgroups(pid);
    Expect(cgmap["memory"] == "/porto");
    KillPorto(api, SIGKILL);
    AsNobody(api);
    expectedWarns++; // Task belongs to invalid subsystem

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    ExpectCorrectCgroups(pid, name);
    ExpectSuccess(api.Destroy(name));

    Say() << "Make sure some data is persistent" << std::endl;
    ExpectSuccess(api.Create(name));

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "memory_limit", "10"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "exit_status", v));
    Expect(v == string("9"));
    ExpectSuccess(api.GetData(name, "oom_killed", v));
    Expect(v == string("true"));
    KillPorto(api, SIGKILL);
    ExpectSuccess(api.GetData(name, "exit_status", v));
    Expect(v == string("9"));
    ExpectSuccess(api.GetData(name, "oom_killed", v));
    Expect(v == string("true"));
    ExpectSuccess(api.Stop(name));

    int expected = 1;
    ExpectSuccess(api.SetProperty(name, "command", "false"));
    ExpectSuccess(api.SetProperty(name, "memory_limit", "0"));
    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.SetProperty(name, "max_respawns", std::to_string(expected)));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");
    KillPorto(api, SIGKILL);
    WaitRespawn(api, name, expected);
    ExpectSuccess(api.GetData(name, "respawn_count", v));
    Expect(v == std::to_string(expected));

    Say() << "Make sure stopped state is persistent" << std::endl;
    ExpectSuccess(api.Destroy(name));
    ExpectSuccess(api.Create(name));
    ShouldHaveValidProperties(api, name);
    ShouldHaveValidData(api, name);
    KillPorto(api, SIGKILL);
    ExpectSuccess(api.GetData(name, "state", v));
    Expect(v == "stopped");
    ShouldHaveValidProperties(api, name);
    ShouldHaveValidData(api, name);

    Say() << "Make sure paused state is persistent" << std::endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ShouldHaveValidRunningData(api, name);
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    v = GetState(pid);
    Expect(v == "S" || v == "R");
    ExpectSuccess(api.Pause(name));
    v = GetState(pid);
    Expect(v == "D");
    KillPorto(api, SIGKILL);
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    v = GetState(pid);
    Expect(v == "D");
    ExpectSuccess(api.Resume(name));
    ShouldHaveValidRunningData(api, name);
    v = GetState(pid);
    Expect(v == "S" || v == "R");
    ExpectSuccess(api.GetData(name, "time", v));
    Expect(v != "0");
    ExpectSuccess(api.Destroy(name));

    if (NetworkEnabled()) {
        Say() << "Make sure network counters are persistent" << std::endl;
        ExpectSuccess(api.Create(name));
        ExpectSuccess(api.SetProperty(name, "command", "bash -c 'wget yandex.ru && sync'"));
        ExpectSuccess(api.Start(name));
        WaitState(api, name, "dead");

        ExpectNonZeroLink(api, name, "net_bytes");
        KillPorto(api, SIGKILL);
        ExpectNonZeroLink(api, name, "net_bytes");

        ExpectSuccess(api.Destroy(name));
    }

    Say() << "Make sure respawn_count ticks after recovery " << std::endl;
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.Start(name));
    Expect(RespawnTicks(api, name) == true);
    KillPorto(api, SIGKILL);
    Expect(RespawnTicks(api, name) == true);
    ExpectSuccess(api.Destroy(name));

    Say() << "Make sure we can recover huge number of containers " << std::endl;
    const size_t nr = config().container().max_total() - 1;

    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectSuccess(api.Create(name));
        ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectSuccess(api.Start(name));
    }

    ExpectFailure(api.Create("max_plus_one"), EError::ResourceNotAvailable);

    KillPorto(api, SIGKILL, 100);

    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == nr + 1);

    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectSuccess(api.Kill(name, SIGKILL));
    }
    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectSuccess(api.Destroy(name));
    }
}

static void TestCgroups(TPortoAPI &api) {
    AsRoot(api);

    Say() << "Make sure we don't remove non-porto cgroups" << std::endl;

    string freezerCg = "/sys/fs/cgroup/freezer/qwerty/asdfg";

    RemakeDir(api, freezerCg);

    KillPorto(api, SIGINT);

    TFolder qwerty(freezerCg);
    Expect(qwerty.Exists() == true);
    Expect(qwerty.Remove() == false);

    Say() << "Make sure we can remove freezed cgroups" << std::endl;

    freezerCg = "/sys/fs/cgroup/freezer/porto/asdf";
    string memoryCg = "/sys/fs/cgroup/memory/porto/asdf";
    string cpuCg = "/sys/fs/cgroup/cpu/porto/asdf";

    RemakeDir(api, freezerCg);
    RemakeDir(api, memoryCg);
    RemakeDir(api, cpuCg);

    int pid = fork();
    if (pid == 0) {
        TFile freezer(freezerCg + "/cgroup.procs");
        ExpectSuccess(freezer.AppendString(std::to_string(getpid())));
        TFile memory(memoryCg + "/cgroup.procs");
        ExpectSuccess(memory.AppendString(std::to_string(getpid())));
        TFile cpu(cpuCg + "/cgroup.procs");
        ExpectSuccess(cpu.AppendString(std::to_string(getpid())));
        execlp("sleep", "sleep", "1000", nullptr);
        abort();
    }

    KillPorto(api, SIGKILL);

    TFolder freezer(freezerCg);
    Expect(freezer.Exists() == false);
    TFolder memory(memoryCg);
    Expect(memory.Exists() == false);
    TFolder cpu(cpuCg);
    Expect(cpu.Exists() == false);
}

static void TestVersion(TPortoAPI &api) {
    string tag, revision;
    ExpectSuccess(api.GetVersion(tag, revision));

    Expect(tag == GIT_TAG);
    Expect(revision == GIT_REVISION);
}

static void TestRemoveDead(TPortoAPI &api) {
    int seconds = 4;
    bool remove;

    std::string v;
    ExpectSuccess(api.GetData("/", "porto_stat[remove_dead]", v));
    Expect(v == std::to_string(0));

    AsRoot(api);

    config().mutable_container()->set_aging_time_s(seconds);
    config().mutable_daemon()->set_rotate_logs_timeout_s(1);
    TFile f("/etc/portod.conf");
    remove = !f.Exists();
    ExpectSuccess(f.WriteStringNoAppend(config().ShortDebugString()));

    KillPorto(api, SIGTERM);

    std::string name = "dead";
    ExpectSuccess(api.Create(name));
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));
    WaitState(api, name, "dead");

    usleep(seconds / 2 * 1000 * 1000);
    std::string state;
    ExpectSuccess(api.GetData(name, "state", state));
    Expect(state == "dead");

    usleep((seconds / 2 + 1) * 1000 * 1000);
    ExpectFailure(api.GetData(name, "state", state), EError::ContainerDoesNotExist);

    ExpectSuccess(api.GetData("/", "porto_stat[remove_dead]", v));
    Expect(v == std::to_string(1));

    if (remove) {
        ExpectSuccess(f.Remove());
    } else {
        config().mutable_container()->set_aging_time_s(60 * 60 * 24 * 7);
        config().mutable_daemon()->set_rotate_logs_timeout_s(60);
        ExpectSuccess(f.WriteStringNoAppend(config().ShortDebugString()));
    }
}

static void TestStats(TPortoAPI &api) {
    if (!needDaemonChecks)
        return;

    AsRoot(api);

    int respawns = WordCount(config().master_log().path(), "Spawned");
    int errors = WordCount(config().slave_log().path(), "Error");
    int warns = WordCount(config().slave_log().path(), "Warning");

    std::string v;
    ExpectSuccess(api.GetData("/", "porto_stat[spawned]", v));
    Expect(v == std::to_string(respawns));

    ExpectSuccess(api.GetData("/", "porto_stat[errors]", v));
    Expect(v == std::to_string(errors));

    ExpectSuccess(api.GetData("/", "porto_stat[warnings]", v));
    Expect(v == std::to_string(warns));

    if (WordCount(config().slave_log().path(),
                  "Task belongs to invalid subsystem") > 1)
        throw string("ERROR: Some task belongs to invalid subsystem!");

    if (respawns - 1 != expectedRespawns)
        throw string("ERROR: Unexpected number of respawns: " + std::to_string(respawns));

    if (errors != expectedErrors)
        throw string("ERROR: Unexpected number of errors: " + std::to_string(errors));

    if (warns != expectedWarns)
        throw string("ERROR: Unexpected number of warnings: " + std::to_string(warns));
}

static void TestPackage(TPortoAPI &api) {
    if (!needDaemonChecks)
        return;

    AsRoot(api);

    Expect(FileExists(config().master_log().path()));
    Expect(FileExists(config().slave_log().path()));
    Expect(FileExists(config().rpc_sock().file().path()));

    Expect(system("stop yandex-porto") == 0);

    Expect(FileExists(config().master_log().path()));
    Expect(FileExists(config().slave_log().path()));
    Expect(FileExists(config().rpc_sock().file().path()) == false);

    system("start yandex-porto");
    WaitPortod(api);
}

int SelfTest(string name, int leakNr) {
    pair<string, std::function<void(TPortoAPI &)>> tests[] = {
        { "root", TestRoot },
        { "data", TestData },
        { "holder", TestHolder },
        { "empty", TestEmpty },
        { "state_machine", TestStateMachine },
        { "exit_status", TestExitStatus },
        { "streams", TestStreams },
        { "ns_cg_tc", TestNsCgTc },
        { "isolate_property", TestIsolateProperty },
        { "env_trim", TestEnvTrim },
        { "env_property", TestEnvProperty },
        { "user_group_property", TestUserGroupProperty },
        { "cwd_property", TestCwdProperty },
        { "stdpath_property", TestStdPathProperty },
        { "root_property", TestRootProperty },
        { "root_readonly", TestRootRdOnlyProperty },
        { "hostname_property", TestHostnameProperty },
        { "bind_property", TestBindProperty },
        { "net_property", TestNetProperty },
        { "allowed_devices_property", TestAllowedDevicesProperty },
        { "capabilities_property", TestCapabilitiesProperty },
        { "limits", TestLimits },
        { "ulimit_property", TestUlimitProperty },
        { "alias", TestAlias },
        { "dynamic", TestDynamic },
        { "permissions", TestPermissions },
        { "respawn_property", TestRespawnProperty },
        { "hierarchy", TestLimitsHierarchy },
        { "leaks", TestLeaks },
        { "perf", TestPerf },

        { "daemon", TestDaemon },
        { "recovery", TestRecovery },
        { "cgroups", TestCgroups },
        { "version", TestVersion },
        { "remove_dead", TestRemoveDead },
        { "stats", TestStats },
        { "package", TestPackage },
    };

    ExpectSuccess(SetHostName(HOSTNAME));

    if (NetworkEnabled())
        subsystems.push_back("net_cls");

    LeakConainersNr = leakNr;

    needDaemonChecks = getenv("NOCHECK") == nullptr;
    try {
        config.Load();
        TPortoAPI api(config().rpc_sock().file().path(), 0);

        if (needDaemonChecks) {
            RestartDaemon(api);

            Expect(WordCount(config().master_log().path(), "Started") == 1);
            Expect(WordCount(config().slave_log().path(), "Started") == 1);
        }

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

        AsRoot(api);

        if (!needDaemonChecks)
            return EXIT_SUCCESS;
    } catch (string e) {
        std::cerr << "EXCEPTION: " << e << std::endl;
        return EXIT_FAILURE;
    }

    std::cerr << "SUCCESS: All tests successfully passed!" << std::endl;
    if (!CanTestLimits())
        std::cerr << "WARNING: Due to missing kernel support, memory_guarantee/cpu_policy has not been tested!" << std::endl;
    if (!IsCfqActive())
        std::cerr << "WARNING: CFQ is not enabled for one of your block devices, skipping io_read and io_write tests" << std::endl;
    if (!NetworkEnabled())
        std::cerr << "WARNING: Network support is not tested" << std::endl;
    if (links.size() == 1)
        std::cerr << "WARNING: Multiple network support is not tested" << std::endl;

    return EXIT_SUCCESS;
}
}
