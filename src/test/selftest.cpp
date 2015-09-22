#include <iostream>
#include <iomanip>
#include <csignal>
#include <cstdio>
#include <climits>
#include <algorithm>

#include "version.hpp"
#include "libporto.hpp"
#include "config.hpp"
#include "value.hpp"
#include "util/netlink.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"
#include "protobuf.hpp"
#include "test.hpp"
#include "rpc.hpp"

#define HOSTNAME "portotest"
const std::string TMPDIR = "/tmp/porto/selftest";

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <linux/capability.h>
}

const std::string oomMemoryLimit = "1000000000";
const std::string oomCommand = "dd if=/dev/zero of=/dev/shm/fill bs=1k count=1024k";

const uint32_t DEF_CLASS_MAX_RATE = -1;
const uint32_t DEF_CLASS_RATE = 1;
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

#define ExpectState(api, name, state) _ExpectState(api, name, state, __LINE__, __func__)
void _ExpectState(TPortoAPI &api, const std::string &name, const std::string &state, size_t line, const char *func) {
    std::string v;
    ExpectApi(api, api.GetData(name, "state", v), 0, line, func);
    _ExpectEq(v, state, line, func);
}

static std::string StartWaitAndGetData(TPortoAPI &api, const std::string &name, const std::string &data) {
    string v;
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, data, v));
    return v;
}

static void RemakeDir(TPortoAPI &api, const TPath &path) {
    TFolder f(path);
    if (f.Exists()) {
        bool drop = geteuid() != 0;
        if (drop)
            AsRoot(api);
        ExpectSuccess(f.Remove(true));
        if (drop)
            AsNobody(api);
    }
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
                ExpectEq(kv.second, "/porto/" + name);
                expected--;
            }
        }
    }
    ExpectEq(expected, 0);
}

static void ShouldHaveOnlyRoot(TPortoAPI &api) {
    std::vector<std::string> containers;

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 1);
    ExpectEq(containers[0], string("/"));
}

static void ShouldHaveValidProperties(TPortoAPI &api, const string &name) {
    string v;

    ExpectApiFailure(api.GetProperty(name, "command[1]", v), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "command[1]", "ls"), EError::InvalidValue);

    ExpectApiSuccess(api.GetProperty(name, "command", v));
    ExpectEq(v, string(""));
    ExpectApiSuccess(api.GetProperty(name, "cwd", v));
    ExpectEq(v, config().container().tmp_dir() + "/" + name);
    ExpectApiSuccess(api.GetProperty(name, "root", v));
    ExpectEq(v, "/");
    ExpectApiSuccess(api.GetProperty(name, "user", v));
    ExpectEq(v, GetDefaultUser());
    ExpectApiSuccess(api.GetProperty(name, "group", v));
    ExpectEq(v, GetDefaultGroup());
    ExpectApiSuccess(api.GetProperty(name, "env", v));
    ExpectEq(v, string(""));
    ExpectApiSuccess(api.GetProperty(name, "memory_guarantee", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetProperty(name, "memory_limit", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetProperty(name, "cpu_policy", v));
    ExpectEq(v, string("normal"));
    ExpectApiSuccess(api.GetProperty(name, "cpu_limit", v));
    ExpectEq(v, "100");
    ExpectApiSuccess(api.GetProperty(name, "cpu_guarantee", v));
    ExpectEq(v, "0");
    if (IsCfqActive()) {
        ExpectApiSuccess(api.GetProperty(name, "io_policy", v));
        ExpectEq(v, "normal");
    }
    if (HaveIoLimit()) {
        ExpectApiSuccess(api.GetProperty(name, "io_limit", v));
        ExpectEq(v, "0");
    }

    for (auto &link : links) {
        ExpectApiSuccess(api.GetProperty(name, "net_guarantee[" + link->GetAlias() + "]", v));
        ExpectEq(v, std::to_string(DEF_CLASS_RATE));
        ExpectApiSuccess(api.GetProperty(name, "net_limit[" + link->GetAlias() + "]", v));
        ExpectEq(v, std::to_string(DEF_CLASS_CEIL));
        ExpectApiSuccess(api.GetProperty(name, "net_priority[" + link->GetAlias() + "]", v));
        ExpectEq(v, std::to_string(DEF_CLASS_NET_PRIO));
        ExpectApiSuccess(api.GetProperty(name, "net", v));
        ExpectEq(v, "inherited");
    }

    ExpectApiSuccess(api.GetProperty(name, "respawn", v));
    ExpectEq(v, string("false"));
    ExpectApiSuccess(api.GetProperty(name, "cpu.smart", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetProperty(name, "memory.low_limit_in_bytes", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetProperty(name, "memory.recharge_on_pgfault", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetProperty(name, "stdin_path", v));
    ExpectEq(v, string("/dev/null"));
    ExpectApiSuccess(api.GetProperty(name, "stdout_path", v));
    ExpectEq(v, config().container().tmp_dir() + "/" + name + "/stdout." + name);
    ExpectApiSuccess(api.GetProperty(name, "stderr_path", v));
    ExpectEq(v, config().container().tmp_dir() + "/" + name + "/stderr." + name);
    ExpectApiSuccess(api.GetProperty(name, "ulimit", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "hostname", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "bind_dns", v));
    ExpectEq(v, "false");
    ExpectApiSuccess(api.GetProperty(name, "allowed_devices", v));
    ExpectEq(v, "a *:* rwm");
    ExpectApiSuccess(api.GetProperty(name, "capabilities", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "recharge_on_pgfault", v));
    ExpectEq(v, "false");
    ExpectApiSuccess(api.GetProperty(name, "isolate", v));
    ExpectEq(v, "true");
    ExpectApiSuccess(api.GetProperty(name, "stdout_limit", v));
    ExpectEq(v, std::to_string(config().container().stdout_limit()));
    ExpectApiSuccess(api.GetProperty(name, "private", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "bind", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "root_readonly", v));
    ExpectEq(v, "false");
    ExpectApiSuccess(api.GetProperty(name, "max_respawns", v));
    ExpectEq(v, "-1");
    ExpectApiSuccess(api.GetProperty(name, "enable_porto", v));
    ExpectEq(v, "true");
}

static void ShouldHaveValidRunningData(TPortoAPI &api, const string &name) {
    string v;

    ExpectApiFailure(api.GetData(name, "__invalid_data__", v), EError::InvalidData);

    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, string("running"));
    ExpectApiFailure(api.GetData(name, "exit_status", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "start_errno", v), EError::InvalidState);

    ExpectApiSuccess(api.GetData(name, "root_pid", v));
    Expect(v != "" && v != "-1" && v != "0");

    ExpectApiSuccess(api.GetData(name, "stdout", v));
    ExpectApiSuccess(api.GetData(name, "stderr", v));
    ExpectApiSuccess(api.GetData(name, "cpu_usage", v));
    ExpectApiSuccess(api.GetData(name, "memory_usage", v));

    if (NetworkEnabled()) {
        ExpectApiSuccess(api.GetData(name, "net_bytes", v));
        ExpectApiSuccess(api.GetData(name, "net_packets", v));
        ExpectApiSuccess(api.GetData(name, "net_drops", v));
        ExpectApiSuccess(api.GetData(name, "net_overlimits", v));
    }

    int intval;
    ExpectApiSuccess(api.GetData(name, "minor_faults", v));
    ExpectSuccess(StringToInt(v, intval));
    Expect(intval > 0);
    ExpectApiSuccess(api.GetData(name, "major_faults", v));
    ExpectSuccess(StringToInt(v, intval));
    Expect(intval >= 0);
    if (HaveMaxRss()) {
        ExpectApiSuccess(api.GetData(name, "max_rss", v));
        ExpectSuccess(StringToInt(v, intval));
        Expect(intval >= 0);
    }

    ExpectApiFailure(api.GetData(name, "oom_killed", v), EError::InvalidState);
    ExpectApiSuccess(api.GetData(name, "respawn_count", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetData(name, "parent", v));
    ExpectEq(v, string("/porto"));
    if (IsCfqActive()) {
        ExpectApiSuccess(api.GetData(name, "io_read", v));
        ExpectApiSuccess(api.GetData(name, "io_write", v));
    }
}

static void ShouldHaveValidData(TPortoAPI &api, const string &name) {
    string v;

    ExpectApiFailure(api.GetData(name, "__invalid_data__", v), EError::InvalidData);

    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, string("stopped"));
    ExpectApiFailure(api.GetData(name, "exit_status", v), EError::InvalidState);
    ExpectApiSuccess(api.GetData(name, "start_errno", v));
    ExpectEq(v, string("-1"));
    ExpectApiFailure(api.GetData(name, "root_pid", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "stdout", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "stderr", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "cpu_usage", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "memory_usage", v), EError::InvalidState);

    if (NetworkEnabled()) {
        ExpectApiFailure(api.GetData(name, "net_bytes", v), EError::InvalidState);
        //ExpectApiFailure(api.GetData(name, "net_bps", v), EError::InvalidState);
        //ExpectApiFailure(api.GetData(name, "net_pps", v), EError::InvalidState);
        ExpectApiFailure(api.GetData(name, "net_packets", v), EError::InvalidState);
        ExpectApiFailure(api.GetData(name, "net_drops", v), EError::InvalidState);
        ExpectApiFailure(api.GetData(name, "net_overlimits", v), EError::InvalidState);
    }
    ExpectApiFailure(api.GetData(name, "minor_faults", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "major_faults", v), EError::InvalidState);
    if (HaveMaxRss()) {
        ExpectApiFailure(api.GetData(name, "max_rss", v), EError::InvalidState);
    }

    ExpectApiFailure(api.GetData(name, "oom_killed", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "respawn_count", v), EError::InvalidState);
    ExpectApiSuccess(api.GetData(name, "parent", v));
    ExpectEq(v, string("/porto"));
    if (IsCfqActive()) {
        ExpectApiFailure(api.GetData(name, "io_read", v), EError::InvalidState);
        ExpectApiFailure(api.GetData(name, "io_write", v), EError::InvalidState);
    }
    ExpectApiSuccess(api.GetProperty(name, "max_respawns", v));
    ExpectEq(v, "-1");
}

static void ExpectTclass(string name, bool exp) {
    string cls = GetCgKnob("net_cls", name, "net_cls.classid");
    ExpectEq(TcClassExist(stoul(cls)), exp);
}

static void TestHolder(TPortoAPI &api) {
    ShouldHaveOnlyRoot(api);

    std::vector<std::string> containers;

    Say() << "Create container A" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Try to create existing container A" << std::endl;
    ExpectApiFailure(api.Create("a"), EError::ContainerAlreadyExists);
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Create container B" << std::endl;
    ExpectApiSuccess(api.Create("b"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 3);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("a"));
    ExpectEq(containers[2], string("b"));
    ShouldHaveValidProperties(api, "b");
    ShouldHaveValidData(api, "b");

    Say() << "Remove container A" << std::endl;
    ExpectApiSuccess(api.Destroy("a"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("b"));

    Say() << "Remove container B" << std::endl;
    ExpectApiSuccess(api.Destroy("b"));

    Say() << "Try to execute operations on invalid container" << std::endl;
    ExpectApiFailure(api.Start("a"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Stop("a"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Pause("a"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Resume("a"), EError::ContainerDoesNotExist);

    string value;
    ExpectApiFailure(api.GetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.SetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.GetData("a", "root_pid", value), EError::ContainerDoesNotExist);

    Say() << "Try to create container with invalid name" << std::endl;
    string name;

    name = "z$";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "/invalid";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "invalid/";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "i//nvalid";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "invalid//";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "invali//d";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = string(128, 'a');
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    name = string(128, 'z');
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    name = string(129, 'z');
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    string parent = "a";
    string child = "a/b";
    ExpectApiFailure(api.Create(child), EError::InvalidValue);
    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.Destroy(parent));
    string v;
    ExpectApiFailure(api.GetData(child, "state", v), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.GetData(parent, "state", v), EError::ContainerDoesNotExist);

    Say() << "Test hierarchy" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("a"));

    ExpectApiSuccess(api.Create("a/b"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 3);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("a"));
    ExpectEq(containers[2], string("a/b"));

    Say() << "Check meta soft limits" << std::endl;

    ExpectApiSuccess(api.Create("a/b/c"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 4);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("a"));
    ExpectEq(containers[2], string("a/b"));
    ExpectEq(containers[3], string("a/b/c"));

    ExpectApiSuccess(api.SetProperty("a/b/c", "command", "sleep 1000"));

    std::string customLimit = std::to_string(1 * 1024 * 1024);

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectApiSuccess(api.GetData("a/b/c", "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetData("a/b", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.GetData("a", "state", v));
    ExpectEq(v, "meta");
    ExpectNeq(GetCgKnob("memory", "a/b/c", "memory.soft_limit_in_bytes"), customLimit);
    ExpectNeq(GetCgKnob("memory", "a/b", "memory.soft_limit_in_bytes"), customLimit);
    ExpectNeq(GetCgKnob("memory", "a", "memory.soft_limit_in_bytes"), customLimit);
    ExpectApiSuccess(api.Stop("a/b/c"));
    ExpectEq(GetCgKnob("memory", "a/b", "memory.soft_limit_in_bytes"), customLimit);
    ExpectEq(GetCgKnob("memory", "a", "memory.soft_limit_in_bytes"), customLimit);

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectNeq(GetCgKnob("memory", "a/b/c", "memory.soft_limit_in_bytes"), customLimit);
    ExpectNeq(GetCgKnob("memory", "a/b", "memory.soft_limit_in_bytes"), customLimit);
    ExpectNeq(GetCgKnob("memory", "a", "memory.soft_limit_in_bytes"), customLimit);
    ExpectApiSuccess(api.Stop("a"));

    Say() << "Make sure parent gets valid state when child starts" << std::endl;

    ExpectApiSuccess(api.SetProperty("a", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectApiSuccess(api.GetData("a/b/c", "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetData("a/b", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.GetData("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a/b/c"));
    ExpectApiSuccess(api.GetData("a/b", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.GetData("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a"));

    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "true"));

    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.GetData("a/b/c", "state", v));
    ExpectEq(v, "stopped");
    ExpectApiSuccess(api.GetData("a/b", "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetData("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a/b"));
    ExpectApiSuccess(api.GetData("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a"));

    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a", "isolate", "true"));

    ExpectApiSuccess(api.Start("a"));
    ExpectApiSuccess(api.GetData("a/b/c", "state", v));
    ExpectEq(v, "stopped");
    ExpectApiSuccess(api.GetData("a/b", "state", v));
    ExpectEq(v, "stopped");
    ExpectApiSuccess(api.GetData("a", "state", v));
    ExpectEq(v, "running");
    ShouldHaveValidRunningData(api, "a");
    ExpectApiSuccess(api.Stop("a"));

    Say() << "Make sure we can have multiple meta parents" << std::endl;

    ExpectApiSuccess(api.Create("x"));
    ExpectApiSuccess(api.Create("x/y"));
    ExpectApiSuccess(api.Create("x/y/z"));
    ExpectApiSuccess(api.SetProperty("x/y/z", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("x/y/z"));
    ExpectApiSuccess(api.Destroy("x"));

    Say() << "Make sure when parent stops/dies children are stopped" << std::endl;

    string state;

    ExpectApiSuccess(api.Start("a"));
    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.Start("a/b/c"));

    ExpectApiSuccess(api.GetData("a/b/c", "state", state));
    ExpectEq(state, "running");
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), true);
    ExpectEq(CgExists("memory", "a/b/c"), true);

    ExpectApiSuccess(api.Stop("a/b"));
    ExpectApiSuccess(api.GetData("a/b/c", "state", state));
    ExpectEq(state, "stopped");
    ExpectApiSuccess(api.GetData("a/b", "state", state));
    ExpectEq(state, "stopped");
    ExpectApiSuccess(api.GetData("a", "state", state));
    ExpectEq(state, "running");
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), false);
    ExpectEq(CgExists("memory", "a/b/c"), false);

    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1"));
    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), true);
    ExpectEq(CgExists("memory", "a/b/c"), true);

    if (NetworkEnabled()) {
        ExpectTclass("a", true);
        ExpectTclass("a/b", true);
        ExpectTclass("a/b/c", true);
    }

    WaitContainer(api, "a/b");
    ExpectApiSuccess(api.GetData("a/b", "state", state));
    ExpectEq(state, "dead");
    ExpectApiSuccess(api.GetData("a/b/c", "state", state));
    ExpectEq(state, "dead");
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), true);
    ExpectEq(CgExists("memory", "a/b/c"), true);

    ExpectApiSuccess(api.Destroy("a/b/c"));
    ExpectApiSuccess(api.Destroy("a/b"));
    ExpectApiSuccess(api.Destroy("a"));

    Say() << "Make sure porto returns valid error code for destroy" << std::endl;
    ExpectApiFailure(api.Destroy("/"), EError::Permission);
    ExpectApiFailure(api.Destroy("doesntexist"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Destroy("z$"), EError::ContainerDoesNotExist);

    Say() << "Make sure we can't start child when parent is dead" << std::endl;

    ExpectApiSuccess(api.Create("parent"));
    ExpectApiSuccess(api.Create("parent/child"));
    ExpectApiSuccess(api.SetProperty("parent", "command", "sleep 1"));
    ExpectApiSuccess(api.SetProperty("parent/child", "command", "sleep 2"));
    ExpectApiSuccess(api.Start("parent"));
    ExpectApiSuccess(api.Start("parent/child"));
    ExpectApiSuccess(api.Stop("parent/child"));
    WaitContainer(api, "parent");
    ExpectApiFailure(api.Start("parent"), EError::InvalidState);
    ExpectApiSuccess(api.Destroy("parent"));

    ShouldHaveOnlyRoot(api);
}

static void TestGet(TPortoAPI &api) {
    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("b"));

    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));

    ExpectApiSuccess(api.Start("a"));

    Say() << "Test combined get" << std::endl;

    std::vector<std::string> name;
    std::vector<std::string> variable;
    std::map<std::string, std::map<std::string, TPortoGetResponse>> result;

    ExpectApiFailure(api.Get(name, variable, result), EError::InvalidValue);
    ExpectEq(result.size(), 0);

    name.push_back("a");
    name.push_back("b");
    ExpectApiFailure(api.Get(name, variable, result), EError::InvalidValue);
    ExpectEq(result.size(), 0);

    name.clear();
    variable.push_back("cwd");
    ExpectApiFailure(api.Get(name, variable, result), EError::InvalidValue);
    ExpectEq(result.size(), 0);

    name.clear();
    variable.clear();

    name.push_back("a");
    name.push_back("b");
    variable.push_back("invalid");
    variable.push_back("user");
    variable.push_back("command");
    variable.push_back("state");
    ExpectApiSuccess(api.Get(name, variable, result));

    ExpectEq(result.size(), 2);
    ExpectEq(result["a"].size(), 4);
    ExpectEq(result["b"].size(), 4);

    std::string user = GetDefaultUser();

    ExpectEq(result["a"]["user"].Value, user);
    ExpectEq(result["a"]["user"].Error, 0);
    ExpectEq(result["a"]["user"].ErrorMsg, "");
    ExpectEq(result["a"]["command"].Value, "sleep 1000");
    ExpectEq(result["a"]["command"].Error, 0);
    ExpectEq(result["a"]["command"].ErrorMsg, "");
    ExpectEq(result["a"]["state"].Value, "running");
    ExpectEq(result["a"]["state"].Error, 0);
    ExpectEq(result["a"]["state"].ErrorMsg, "");
    ExpectEq(result["a"]["invalid"].Value, "");
    ExpectEq(result["a"]["invalid"].Error, (int)EError::InvalidValue);
    ExpectNeq(result["a"]["invalid"].ErrorMsg, "");

    ExpectEq(result["b"]["user"].Value, user);
    ExpectEq(result["b"]["user"].Error, 0);
    ExpectEq(result["b"]["user"].ErrorMsg, "");
    ExpectEq(result["b"]["command"].Value, "");
    ExpectEq(result["b"]["command"].Error, 0);
    ExpectEq(result["b"]["command"].ErrorMsg, "");
    ExpectEq(result["b"]["state"].Value, "stopped");
    ExpectEq(result["b"]["state"].Error, 0);
    ExpectEq(result["b"]["state"].ErrorMsg, "");
    ExpectEq(result["b"]["invalid"].Value, "");
    ExpectEq(result["b"]["invalid"].Error, (int)EError::InvalidValue);
    ExpectNeq(result["b"]["invalid"].ErrorMsg, "");

    ExpectApiSuccess(api.Destroy("a"));
    ExpectApiSuccess(api.Destroy("b"));
}

static void TestMeta(TPortoAPI &api) {
    std::string state;
    ShouldHaveOnlyRoot(api);

    std::vector<std::string> isolateVals = { "true", "false" };

    for (auto isolate : isolateVals) {
        Say() << "Test meta state machine with isolate = " << isolate << std::endl;

        ExpectApiSuccess(api.Create("a"));
        ExpectApiSuccess(api.Create("a/b"));

        ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 2"));

        ExpectApiSuccess(api.SetProperty("a", "isolate", isolate));
        ExpectApiSuccess(api.SetProperty("a/b", "isolate", "true"));

        ExpectApiSuccess(api.GetData("a", "state", state));
        ExpectEq(state, "stopped");
        ExpectApiSuccess(api.GetData("a/b", "state", state));
        ExpectEq(state, "stopped");

        ExpectApiSuccess(api.Start("a/b"));
        ExpectApiSuccess(api.GetData("a", "state", state));
        ExpectEq(state, "meta");
        ExpectApiSuccess(api.GetData("a/b", "state", state));
        ExpectEq(state, "running");

        WaitContainer(api, "a/b");
        ExpectApiSuccess(api.GetData("a", "state", state));
        ExpectEq(state, "meta");
        ExpectApiSuccess(api.GetData("a/b", "state", state));
        ExpectEq(state, "dead");

        ExpectApiSuccess(api.Stop("a/b"));
        ExpectApiSuccess(api.GetData("a", "state", state));
        ExpectEq(state, "meta");
        ExpectApiSuccess(api.GetData("a/b", "state", state));
        ExpectEq(state, "stopped");

        ExpectApiSuccess(api.Stop("a"));
        ExpectApiSuccess(api.GetData("a", "state", state));
        ExpectEq(state, "stopped");

        ExpectApiSuccess(api.Destroy("a"));
    }
}

static void TestEmpty(TPortoAPI &api) {
    Say() << "Make sure we can't start empty container" << std::endl;
    ExpectApiSuccess(api.Create("b"));
    ExpectApiFailure(api.Start("b"), EError::InvalidValue);
    ExpectApiSuccess(api.Destroy("b"));
}

static bool TaskRunning(const string &pid) {
    int p = stoi(pid);
    if (kill(p, 0))
        return false;
    auto state = GetState(pid);
    return state != "Z" && state != "X";
}

static bool TaskZombie(const string &pid) {
    return GetState(pid) == "Z";
}

static void TestExitStatus(TPortoAPI &api) {
    string pid;
    string ret;

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check exit status of 'false'" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "false"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "exit_status", ret));
    ExpectEq(ret, string("256"));
    ExpectApiSuccess(api.GetData(name, "oom_killed", ret));
    ExpectEq(ret, string("false"));
    ExpectApiFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check exit status of 'true'" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "true"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "exit_status", ret));
    ExpectEq(ret, string("0"));
    ExpectApiSuccess(api.GetData(name, "oom_killed", ret));
    ExpectEq(ret, string("false"));
    ExpectApiFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check exit status of invalid command" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectApiSuccess(api.SetProperty(name, "cwd", "/"));
    ExpectApiFailure(api.Start(name), EError::InvalidValue);
    ExpectApiFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "oom_killed", ret), EError::InvalidState);
    ExpectApiSuccess(api.GetData(name, "start_errno", ret));
    ExpectEq(ret, string("2"));

    Say() << "Check exit status of invalid directory" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "cwd", "/__invalid__dir__"));
    ExpectApiFailure(api.Start(name), EError::InvalidValue);
    ExpectApiFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectApiFailure(api.GetData(name, "oom_killed", ret), EError::InvalidState);
    ExpectApiSuccess(api.GetData(name, "start_errno", ret));
    ExpectEq(ret, string("2"));

    Say() << "Check exit status when killed by signal" << std::endl;
    ExpectApiSuccess(api.Destroy(name));
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    kill(stoi(pid), SIGKILL);
    WaitContainer(api, name);
    WaitProcessExit(pid);
    ExpectApiSuccess(api.GetData(name, "exit_status", ret));
    ExpectEq(ret, string("9"));
    ExpectApiSuccess(api.GetData(name, "oom_killed", ret));
    ExpectEq(ret, string("false"));
    ExpectApiFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check oom_killed property" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", oomCommand));
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "10"));
    // limit is so small we can't even start process
    ExpectApiFailure(api.Start(name), EError::InvalidValue);

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", oomMemoryLimit));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name, 60);
    ExpectApiSuccess(api.GetData(name, "exit_status", ret));
    ExpectEq(ret, string("9"));
    ExpectApiSuccess(api.GetData(name, "oom_killed", ret));
    ExpectEq(ret, string("true"));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestStreams(TPortoAPI &api) {
    string ret;

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Make sure stdout works" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", ret));
    ExpectEq(ret, string("out\n"));
    ExpectApiSuccess(api.GetData(name, "stderr", ret));
    ExpectEq(ret, string(""));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure stderr works" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", ret));
    ExpectEq(ret, string(""));
    ExpectApiSuccess(api.GetData(name, "stderr", ret));
    ExpectEq(ret, string("err\n"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestNsCgTc(TPortoAPI &api) {
    string pid;

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Spawn long running task" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);

    AsRoot(api);
    Say() << "Check that portod doesn't leak fds" << std::endl;
    struct dirent **lst;
    std::string path = "/proc/" + pid + "/fd/";
    int nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    ExpectEq(nr, 2 + 4);

    Say() << "Check that task namespaces are correct" << std::endl;
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(pid, "pid"));
    ExpectNeq(GetNamespace("self", "mnt"), GetNamespace(pid, "mnt"));
    ExpectNeq(GetNamespace("self", "ipc"), GetNamespace(pid, "ipc"));
    ExpectEq(GetNamespace("self", "net"), GetNamespace(pid, "net"));
    //ExpectEq(GetNamespace("self", "user"), GetNamespace(pid, "user"));
    ExpectNeq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));

    Say() << "Check that task cgroups are correct" << std::endl;
    auto cgmap = GetCgroups("self");
    for (auto name : cgmap) {
        // skip systemd cgroups
        if (name.first.find("systemd") != string::npos)
            continue;

        ExpectEq(name.second, "/");
    }

    ExpectCorrectCgroups(pid, name);
    AsNobody(api);

    string root_cls;
    string leaf_cls;
    if (NetworkEnabled()) {
        root_cls = GetCgKnob("net_cls", "/", "net_cls.classid");
        leaf_cls = GetCgKnob("net_cls", name, "net_cls.classid");

        ExpectNeq(root_cls, "0");
        ExpectNeq(leaf_cls, "0");
        ExpectNeq(root_cls, leaf_cls);

        ExpectEq(TcClassExist(stoul(root_cls)), true);
        ExpectEq(TcClassExist(stoul(leaf_cls)), true);
    }

    ExpectApiSuccess(api.Stop(name));
    WaitProcessExit(pid);

    if (NetworkEnabled()) {
        ExpectEq(TcClassExist(stoul(leaf_cls)), false);

        Say() << "Check that destroying container removes tclass" << std::endl;
        ExpectApiSuccess(api.Start(name));
        ExpectEq(TcClassExist(stoul(root_cls)), true);
        ExpectEq(TcClassExist(stoul(leaf_cls)), true);
        ExpectApiSuccess(api.Destroy(name));
        ExpectEq(TaskRunning(pid), false);
        ExpectEq(TcClassExist(stoul(leaf_cls)), false);
        ExpectApiSuccess(api.Create(name));
    }

    Say() << "Check that hierarchical task cgroups are correct" << std::endl;

    string child = name + "/b";
    ExpectApiSuccess(api.Create(child));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectCorrectCgroups(pid, name);

    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));
    ExpectApiSuccess(api.GetData(child, "root_pid", pid));
    ExpectCorrectCgroups(pid, child);

    string parent;
    ExpectApiSuccess(api.GetData(child, "parent", parent));
    ExpectEq(parent, name);

    ExpectApiSuccess(api.Destroy(child));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestIsolateProperty(TPortoAPI &api) {
    string ret;

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Make sure PID isolation works" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "isolate", "false"));

    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", ret));
    ExpectNeq(ret, string("1\n"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "ps aux"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", ret));
    ExpectNeq(std::count(ret.begin(), ret.end(), '\n'), 2);
    ExpectApiSuccess(api.Stop(name));


    ExpectApiSuccess(api.SetProperty(name, "isolate", "true"));
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == "1\n" || ret == "2\n");
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "ps aux"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", ret));
    Expect(std::count(ret.begin(), ret.end(), '\n') < 4);

    if (NetworkEnabled()) {
        Say() << "Make sure container has correct network class" << std::endl;

        string handle = GetCgKnob("net_cls", name, "net_cls.classid");
        ExpectNeq(handle, "0");

        ExpectEq(TcClassExist(stoul(handle)), true);
        ExpectApiSuccess(api.Stop(name));
        ExpectEq(TcClassExist(stoul(handle)), false);
    }
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure isolate works correctly with meta parent" << std::endl;
    string pid;

    ExpectApiSuccess(api.Create("meta"));
    ExpectApiSuccess(api.SetProperty("meta", "isolate", "false"));

    ExpectApiSuccess(api.Create("meta/test"));
    ExpectApiSuccess(api.SetProperty("meta/test", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("meta/test", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("meta/test"));
    ExpectApiSuccess(api.GetData("meta/test", "root_pid", pid));
    AsRoot(api);
    ExpectEq(GetNamespace("self", "pid"), GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop("meta/test"));

    ExpectApiSuccess(api.SetProperty("meta/test", "isolate", "true"));
    ExpectApiSuccess(api.SetProperty("meta/test", "command", "sh -c 'ps aux; sleep 1000'"));
    ExpectApiSuccess(api.Start("meta/test"));
    ExpectApiSuccess(api.GetData("meta/test", "root_pid", pid));
    AsRoot(api);
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop("meta/test"));

    ExpectApiSuccess(api.Destroy("meta/test"));
    ExpectApiSuccess(api.Destroy("meta"));

    ExpectApiSuccess(api.Create("test"));
    ExpectApiSuccess(api.Create("test/meta"));
    ExpectApiSuccess(api.SetProperty("test/meta", "isolate", "false"));
    ExpectApiSuccess(api.Create("test/meta/test"));

    ExpectApiSuccess(api.SetProperty("test", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("test"));

    ExpectApiSuccess(api.SetProperty("test/meta/test", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("test/meta/test"));
    ExpectApiSuccess(api.GetData("test", "root_pid", pid));
    ExpectApiSuccess(api.GetData("test/meta/test", "root_pid", ret));
    AsRoot(api);
    ExpectNeq(GetNamespace(ret, "pid"), GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop("test/meta/test"));

    ExpectApiSuccess(api.SetProperty("test/meta/test", "isolate", "false"));
    ExpectApiSuccess(api.Start("test/meta/test"));
    ExpectApiSuccess(api.GetData("test", "root_pid", pid));
    ExpectApiSuccess(api.GetData("test/meta/test", "root_pid", ret));
    AsRoot(api);
    ExpectEq(GetNamespace(ret, "pid"), GetNamespace(pid, "pid"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop("test/meta/test"));

    ExpectApiSuccess(api.Destroy("test/meta/test"));
    ExpectApiSuccess(api.Destroy("test/meta"));
    ExpectApiSuccess(api.Destroy("test"));

    Say() << "Make sure isolate works correctly with isolate=true in meta containers" << std::endl;
    ExpectApiSuccess(api.Create("iss"));
    ExpectApiSuccess(api.SetProperty("iss", "isolate", "false"));

    ExpectApiSuccess(api.Create("iss/container"));
    ExpectApiSuccess(api.SetProperty("iss/container", "isolate", "true"));

    ExpectApiSuccess(api.Create("iss/container/hook1"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook1", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook1", "command", "sleep 1000"));
    ExpectApiSuccess(api.Create("iss/container/hook2"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook2", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook2", "command", "sleep 1000"));

    ExpectApiSuccess(api.Start("iss/container/hook1"));
    ExpectApiSuccess(api.Start("iss/container/hook2"));

    std::string rootPid, hook1Pid, hook2Pid;
    ExpectApiSuccess(api.GetData("iss/container/hook1", "root_pid", hook1Pid));
    ExpectApiSuccess(api.GetData("iss/container/hook2", "root_pid", hook2Pid));

    std::string state;
    ExpectApiSuccess(api.GetData("iss/container", "state", state));
    ExpectEq(state, "meta");

    AsRoot(api);
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook1Pid, "pid"));
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook2Pid, "pid"));
    ExpectEq(GetNamespace(hook1Pid, "pid"), GetNamespace(hook2Pid, "pid"));
    AsNobody(api);

    ExpectApiSuccess(api.Stop("iss/container"));

    Say() << "Make sure isolate works correctly with isolate=true and chroot in meta containers" << std::endl;

    TPath path(TMPDIR + "/" + name);

    RemakeDir(api, path);
    AsRoot(api);
    BootstrapCommand("/bin/sleep", path.ToString());
    path.Chown("nobody", "nogroup");
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty("iss/container", "root", path.ToString()));
    ExpectApiSuccess(api.SetProperty("iss/container/hook1", "command", "/sleep 1000"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook2", "command", "/sleep 1000"));

    ExpectApiSuccess(api.Start("iss/container/hook1"));
    ExpectApiSuccess(api.Start("iss/container/hook2"));

    ExpectApiSuccess(api.GetData("iss/container/hook1", "root_pid", hook1Pid));
    ExpectApiSuccess(api.GetData("iss/container/hook2", "root_pid", hook2Pid));

    ExpectApiSuccess(api.GetData("iss/container", "state", state));
    ExpectEq(state, "meta");

    AsRoot(api);
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook1Pid, "pid"));
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook2Pid, "pid"));
    ExpectEq(GetNamespace(hook1Pid, "pid"), GetNamespace(hook2Pid, "pid"));
    AsNobody(api);

    ExpectApiSuccess(api.Destroy("iss"));

    Say() << "Make sure kill correctly works with isolate = false" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.SetProperty("a", "isolate", "true"));

    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));
    ExpectApiSuccess(api.Start("a/b"));

    ExpectApiSuccess(api.Create("a/c"));
    ExpectApiSuccess(api.SetProperty("a/c", "command", "bash -c 'nohup sleep 1000 & nohup sleep 1000 & sleep 1000'"));
    ExpectApiSuccess(api.SetProperty("a/c", "isolate", "false"));
    ExpectApiSuccess(api.Start("a/c"));

    ExpectApiSuccess(api.GetData("a/c", "root_pid", pid));
    ExpectApiSuccess(api.Kill("a/c", SIGKILL));
    WaitContainer(api, "a/c");

    WaitProcessExit(pid);
    kill(stoi(pid), 0);
    ExpectEq(errno, ESRCH);

    ExpectApiSuccess(api.GetData("a", "state", state));
    ExpectEq(state, "meta");
    ExpectApiSuccess(api.GetData("a/b", "state", state));
    ExpectEq(state, "running");
    ExpectApiSuccess(api.GetData("a/c", "state", state));
    ExpectEq(state, "dead");
    ExpectApiSuccess(api.Destroy("a"));
}

static void TestContainerNamespaces(TPortoAPI &api) {
    std::string val;

    Say() << "Test container namespaces" << std::endl;

    Say() << "Check default value" << std::endl;
    ExpectApiSuccess(api.Create("c"));
    ExpectApiSuccess(api.GetProperty("c", "porto_namespace", val));
    ExpectEq(val, "");

    Say() << "Check inheritance" << std::endl;
    ExpectApiSuccess(api.SetProperty("c", "porto_namespace", "my-prefix-"));
    ExpectApiSuccess(api.GetProperty("c", "porto_namespace", val));
    ExpectApiSuccess(api.Create("c/d"));
    ExpectApiSuccess(api.GetProperty("c/d", "porto_namespace", val));
    ExpectEq(val, "");
    ExpectApiSuccess(api.SetProperty("c/d", "porto_namespace", "second-prefix-"));
    ExpectApiSuccess(api.GetProperty("c/d", "porto_namespace", val));
    ExpectEq(val, "second-prefix-");

    Say() << "Check simple prefix" << std::endl;
    ExpectApiSuccess(api.SetProperty("c", "porto_namespace", "simple-prefix-"));
    ExpectApiSuccess(api.SetProperty("c/d", "command", "portoctl create test"));
    AsRoot(api);
    ExpectApiSuccess(api.SetProperty("c/d", "user", "root"));
    ExpectApiSuccess(api.Start("c/d"));
    WaitContainer(api, "c/d");

    ExpectApiSuccess(api.Destroy("simple-prefix-second-prefix-test"));
    ExpectApiSuccess(api.Stop("c/d"));
    ExpectApiSuccess(api.Stop("c"));

    Say() << "Check container prefix" << std::endl;
    ExpectApiSuccess(api.SetProperty("c", "porto_namespace", "c/"));
    ExpectApiSuccess(api.SetProperty("c/d", "command", "portoctl create test"));
    ExpectApiSuccess(api.Start("c/d"));
    WaitContainer(api, "c/d");
    ExpectApiSuccess(api.Destroy("c/second-prefix-test"));
    ExpectApiSuccess(api.Stop("c/d"));

    Say() << "Check absolute name" << std::endl;
    ExpectApiSuccess(api.Start("c/d"));
    WaitContainer(api, "c/d");
    ExpectApiSuccess(api.GetData("c/second-prefix-test", "absolute_name", val));
    ExpectEq(val, "c/second-prefix-test");
    ExpectApiSuccess(api.Stop("c/d"));
    ExpectApiSuccess(api.Destroy("c/d"));
    ExpectApiSuccess(api.Destroy("c"));
}

static void TestEnvTrim(TPortoAPI &api) {
    string val;
    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check property trimming" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "env", ""));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "");

    ExpectApiSuccess(api.SetProperty(name, "env", " "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "");

    ExpectApiSuccess(api.SetProperty(name, "env", "    "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "");

    ExpectApiSuccess(api.SetProperty(name, "env", " a"));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "a");

    ExpectApiSuccess(api.SetProperty(name, "env", "b "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "b");

    ExpectApiSuccess(api.SetProperty(name, "env", " c "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "c");

    ExpectApiSuccess(api.SetProperty(name, "env", "     d     "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "d");

    ExpectApiSuccess(api.SetProperty(name, "env", "    e"));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "e");

    ExpectApiSuccess(api.SetProperty(name, "env", "f    "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "f");

    string longProperty = string(10 * 1024, 'x');
    ExpectApiSuccess(api.SetProperty(name, "env", longProperty));
    ExpectApiSuccess(api.GetProperty(name, "env", val));

    ExpectApiSuccess(api.Destroy(name));
}

static void ExpectEnv(TPortoAPI &api,
                      const std::string &name,
                      const std::string &env,
                      const char expected[],
                      size_t expectedLen) {
    string pid;

    ExpectApiSuccess(api.SetProperty(name, "env", env));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    string ret = GetEnv(pid);

    ExpectEq(memcmp(expected, ret.data(), expectedLen), 0);
    ExpectApiSuccess(api.Stop(name));
}

static void TestEnvProperty(TPortoAPI &api) {
    string name = "a";
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    AsRoot(api);

    Say() << "Check default environment" << std::endl;

    static const char empty_env[] =
        "HOME=/place/porto/a\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "PORTO_HOST=" HOSTNAME "\0"
        "PORTO_NAME=a\0"
        "USER=nobody\0"
        "container=lxc\0";
    ExpectEnv(api, name, "", empty_env, sizeof(empty_env));

    Say() << "Check user-defined environment" << std::endl;
    static const char ab_env[] =
        "a=b\0"
        "c=d\0"
        "HOME=/place/porto/a\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "PORTO_HOST=" HOSTNAME "\0"
        "PORTO_NAME=a\0"
        "USER=nobody\0"
        "container=lxc\0";

    ExpectEnv(api, name, "a=b;c=d;", ab_env, sizeof(ab_env));
    ExpectEnv(api, name, "a=b;;c=d;", ab_env, sizeof(ab_env));

    static const char asb_env[] =
        "a=e;b\0"
        "c=d\0"
        "HOME=/place/porto/a\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        "PORTO_HOST=" HOSTNAME "\0"
        "PORTO_NAME=a\0"
        "USER=nobody\0"
        "container=lxc\0";
    ExpectEnv(api, name, "a=e\\;b;c=d;", asb_env, sizeof(asb_env));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep $N"));
    ExpectApiSuccess(api.SetProperty(name, "env", "N=1"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestUserGroupProperty(TPortoAPI &api) {
    int uid, gid;
    string pid;

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check default user & group" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    ExpectEq(uid, UserUid(GetDefaultUser()));
    ExpectEq(gid, GroupGid(GetDefaultGroup()));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check custom user & group" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    ExpectApiFailure(api.SetProperty(name, "user", "daemon"), EError::Permission);
    ExpectApiFailure(api.SetProperty(name, "group", "bin"), EError::Permission);

    string user, group;
    ExpectApiSuccess(api.GetProperty(name, "user", user));
    ExpectApiSuccess(api.GetProperty(name, "group", group));
    ExpectApiSuccess(api.SetProperty(name, "user", user));
    ExpectApiSuccess(api.SetProperty(name, "group", group));

    AsRoot(api);
    ExpectApiSuccess(api.SetProperty(name, "user", "daemon"));
    ExpectApiSuccess(api.SetProperty(name, "group", "bin"));
    AsNobody(api);

    ExpectApiFailure(api.Start(name), EError::Permission);

    AsRoot(api);
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    ExpectEq(uid, UserUid("daemon"));
    ExpectEq(gid, GroupGid("bin"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check integer user & group" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "user", "123"));
    ExpectApiSuccess(api.SetProperty(name, "group", "234"));
    ExpectApiSuccess(api.GetProperty(name, "user", user));
    ExpectApiSuccess(api.GetProperty(name, "group", group));
    ExpectEq(user, "123");
    ExpectEq(group, "234");

    ExpectApiSuccess(api.Destroy(name));
    AsNobody(api);
}

static void TestCwdProperty(TPortoAPI &api) {
    string pid;
    string cwd;
    string portodPid, portodCwd;

    AsRoot(api);

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    TFile portod(config().slave_pid().path());
    (void)portod.AsString(portodPid);
    portodCwd = GetCwd(portodPid);

    Say() << "Check default working directory" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    cwd = GetCwd(pid);

    string prefix = config().container().tmp_dir();

    ExpectNeq(cwd, portodCwd);
    ExpectEq(cwd, prefix + "/" + name);

    ExpectEq(access(cwd.c_str(), F_OK), 0);
    ExpectApiSuccess(api.Stop(name));
    ExpectNeq(access(cwd.c_str(), F_OK), 0);
    ExpectApiSuccess(api.Destroy(name));

    ExpectApiSuccess(api.Create("b"));
    ExpectApiSuccess(api.SetProperty("b", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("b"));
    ExpectApiSuccess(api.GetData("b", "root_pid", pid));
    string bcwd = GetCwd(pid);
    ExpectApiSuccess(api.Destroy("b"));

    ExpectNeq(bcwd, portodCwd);
    ExpectEq(bcwd, prefix + "/b");
    ExpectNeq(bcwd, cwd);

    Say() << "Check user defined working directory" << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "cwd", "/tmp"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    ExpectEq(access(("/tmp/stdout." + name).c_str(), F_OK), 0);
    ExpectEq(access(("/tmp/stderr." + name).c_str(), F_OK), 0);

    cwd = GetCwd(pid);

    ExpectEq(cwd, "/tmp");
    ExpectEq(access("/tmp", F_OK), 0);
    ExpectApiSuccess(api.Stop(name));
    ExpectEq(access("/tmp", F_OK), 0);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check working directory of meta parent/child" << std::endl;
    std::string parent = "parent", child = "parent/child";

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(child, "cwd", "/tmp"));
    ExpectApiSuccess(api.SetProperty(child, "command", "pwd"));
    ExpectApiSuccess(api.SetProperty(child, "isolate", "false"));
    auto s = StartWaitAndGetData(api, child, "stdout");
    ExpectEq(StringTrim(s), "/tmp");
    ExpectApiSuccess(api.Destroy(parent));

    AsNobody(api);
}

static void TestStdPathProperty(TPortoAPI &api) {
    string pid;
    string name = "a";
    std::string stdinPath, stdoutPath, stderrPath;

    AsRoot(api);

    ExpectApiSuccess(api.Create(name));

    Say() << "Check default stdin/stdout/stderr" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.GetProperty(name, "stdout_path", stdoutPath));
    ExpectApiSuccess(api.GetProperty(name, "stderr_path", stderrPath));

    Expect(!FileExists(stdoutPath));
    Expect(!FileExists(stderrPath));
    ExpectApiSuccess(api.Start(name));
    Expect(FileExists(stdoutPath));
    Expect(FileExists(stderrPath));

    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectEq(ReadLink("/proc/" + pid + "/fd/0"), "/dev/null");
    ExpectEq(ReadLink("/proc/" + pid + "/fd/1"), stdoutPath);
    ExpectEq(ReadLink("/proc/" + pid + "/fd/2"), stderrPath);
    ExpectApiSuccess(api.Stop(name));

    Expect(!FileExists(stdoutPath));
    Expect(!FileExists(stderrPath));

    Say() << "Check custom stdin/stdout/stderr" << std::endl;
    stdinPath = "/tmp/a_stdin";
    stdoutPath = "/tmp/a_stdout";
    stderrPath = "/tmp/a_stderr";

    TFile stdinFile(stdinPath);
    (void)stdinFile.Remove();
    TFile stdoutFile(stdoutPath);
    (void)stdoutFile.Remove();
    TFile stderrFile(stderrPath);
    (void)stderrFile.Remove();

    TFile f(stdinPath);
    ExpectSuccess(f.Touch());
    ExpectSuccess(f.WriteStringNoAppend("hi"));

    ExpectApiSuccess(api.SetProperty(name, "stdin_path", "/tmp/a_stdin"));
    ExpectApiSuccess(api.SetProperty(name, "stdout_path", "/tmp/a_stdout"));
    ExpectApiSuccess(api.SetProperty(name, "stderr_path", "/tmp/a_stderr"));
    Expect(!FileExists(stdoutPath));
    Expect(!FileExists(stderrPath));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectEq(ReadLink("/proc/" + pid + "/fd/0"), "/tmp/a_stdin");
    ExpectEq(ReadLink("/proc/" + pid + "/fd/1"), "/tmp/a_stdout");
    ExpectEq(ReadLink("/proc/" + pid + "/fd/2"), "/tmp/a_stderr");
    ExpectApiSuccess(api.Stop(name));
    Expect(FileExists(stdinPath));
    Expect(FileExists(stdoutPath));
    Expect(FileExists(stderrPath));

    Say() << "Make sure custom stdin is not removed" << std::endl;
    string ret;
    ExpectApiSuccess(api.SetProperty(name, "command", "cat"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", ret));
    ExpectEq(ret, string("hi"));

    ExpectApiSuccess(api.Destroy(name));

    Expect(FileExists(stdinPath));
    Expect(FileExists(stdoutPath));
    Expect(FileExists(stderrPath));

    Expect(FileExists(stdinPath));
    Expect(FileExists(stdoutPath));
    Expect(FileExists(stderrPath));

    AsNobody(api);
}

struct TMountInfo {
    std::string flags;
    std::string source;
};

static map<string, TMountInfo> ParseMountinfo(string s) {
    map<string, TMountInfo> m;
    vector<string> lines;

    TError error = SplitString(s, '\n', lines);
    if (error)
        throw error.GetMsg();

    for (auto &line : lines) {
        vector<string> tok;
        TError error = SplitString(line, ' ', tok);
        if (error)
            throw error.GetMsg();

        if (tok.size() <= 5)
            throw string("Invalid mount: ") + line;

        TMountInfo i;
        i.flags = tok[5];

        int sep = 6;
        while (tok[sep] != "-")
            sep++;

        i.source = tok[sep + 2];

        m[tok[4]] = i;
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
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.GetProperty(name, "root_readonly", ROnly));
    ExpectEq(ROnly, "false");

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));
    AsRoot(api);
    BootstrapCommand("/usr/bin/touch", path.ToString());
    BootstrapCommand("/bin/cat", path.ToString(), false);
    path.Chown("nobody", "nogroup");
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/touch test"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "exit_status", ret));
    ExpectEq(ret, string("0"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "root_readonly", "true"));
    ExpectApiSuccess(api.SetProperty(name, "command", "/touch test2"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "exit_status", ret));
    ExpectNeq(ret, string("0"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure pivot_root works and we don't leak host mount points" << std::endl;
    std::set<std::string> expected = {
        // restricted proc
        "/proc/sysrq-trigger",
        "/proc/irq",
        "/proc/bus",
        "/proc/sys",
        "/proc/kcore",

        // dev
        "/dev",
        "/dev/shm",
        "/dev/pts",

        "/proc",
        "/sys",
        "/",
    };

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));
    ExpectApiSuccess(api.SetProperty(name, "root_readonly", "true"));
    ExpectApiSuccess(api.SetProperty(name, "bind_dns", "false"));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
    ExpectApiSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    auto v = StartWaitAndGetData(api, name, "stdout");
    auto m = ParseMountinfo(v);
    ExpectEq(m.size(), expected.size());
    for (auto pair : m)
        Expect(expected.find(pair.first) != expected.end());

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
};

unsigned long GetInode(const TPath &path) {
    struct stat st;
    ExpectEq(stat(path.ToString().c_str(), &st), 0);
    return st.st_ino;
}

static void TestRootProperty(TPortoAPI &api) {
    string pid;
    string v;

    string name = "a";
    string path = TMPDIR + "/" + name;

    Say() << "Make sure root is empty" << std::endl;

    ExpectApiSuccess(api.Create(name));
    RemakeDir(api, path);

    ExpectApiSuccess(api.SetProperty(name, "command", "ls"));
    ExpectApiSuccess(api.SetProperty(name, "root", path));

    ExpectApiFailure(api.Start(name), EError::InvalidValue);
    ExpectApiSuccess(api.GetData(name, "start_errno", v));
    ExpectEq(v, string("2"));

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check filesystem isolation" << std::endl;

    ExpectApiSuccess(api.Create(name));

    RemakeDir(api, path);

    AsRoot(api);
    BootstrapCommand("/bin/sleep", path, false);
    BootstrapCommand("/bin/pwd", path, false);
    BootstrapCommand("/bin/ls", path, false);
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    string bindDns;

    ExpectApiSuccess(api.GetProperty(name, "bind_dns", bindDns));
    ExpectEq(bindDns, "false");

    ExpectApiSuccess(api.SetProperty(name, "root", path));

    string cwd;
    ExpectApiSuccess(api.GetProperty(name, "cwd", cwd));
    ExpectEq(cwd, "/");

    ExpectApiSuccess(api.GetProperty(name, "bind_dns", bindDns));
    ExpectEq(bindDns, "true");

    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    // root or cwd may have / but it actually points to correct path,
    // test inodes instead
    AsRoot(api);
    ExpectEq(GetInode("/proc/" + pid + "/cwd"), GetInode(path));
    ExpectEq(GetInode("/proc/" + pid + "/root"), GetInode(path));
    AsNobody(api);

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/pwd"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);

    ExpectApiSuccess(api.GetData(name, "stdout", v));
    ExpectEq(v, string("/\n"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check /dev layout" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "command", "/ls -1 /dev"));
    v = StartWaitAndGetData(api, name, "stdout");

    vector<string> devs = { "null", "zero", "full", "urandom", "random", "console" };
    vector<string> other = { "ptmx", "pts", "shm", "fd" };
    vector<string> tokens;
    TError error = SplitString(v, '\n', tokens);
    if (error)
        throw error.GetMsg();

    ExpectEq(devs.size() + other.size(), tokens.size());
    for (auto &dev : devs)
        Expect(std::find(tokens.begin(), tokens.end(), dev) != tokens.end());

    ExpectApiSuccess(api.Stop(name));

    Say() << "Check /proc restrictions" << std::endl;

    RemakeDir(api, path);
    AsRoot(api);
    BootstrapCommand("/bin/cat", path, false);
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    v = StartWaitAndGetData(api, name, "stdout");

    auto m = ParseMountinfo(v);
    ExpectNeq(m["/etc/resolv.conf"].flags.find("ro,"), string::npos);
    ExpectNeq(m["/etc/hosts"].flags.find("ro,"), string::npos);
    ExpectNeq(m["/sys"].flags.find("ro,"), string::npos);
    ExpectNeq(m["/proc/sys"].flags.find("ro,"), string::npos);
    ExpectNeq(m["/proc/sysrq-trigger"].flags.find("ro,"), string::npos);
    ExpectNeq(m["/proc/irq"].flags.find("ro,"), string::npos);
    ExpectNeq(m["/proc/bus"].flags.find("ro,"), string::npos);

    ExpectApiSuccess(api.Stop(name));

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

    ExpectApiSuccess(api.SetProperty(name, "root", "/"));
    ExpectApiSuccess(api.SetProperty(name, "command", "ls -1 " + cwd));

    v = StartWaitAndGetData(api, name, "stdout");
    ExpectEq(v, "stderr." + name + "\nstdout." + name + "\n");

    ExpectApiSuccess(api.Destroy(name));
}

static string GetHostname() {
    char buf[1024];
    ExpectEq(gethostname(buf, sizeof(buf)), 0);
    return buf;
}

static void TestHostnameProperty(TPortoAPI &api) {
    string pid, v;
    string name = "a";
    string host = "porto_" + name;
    string path = TMPDIR + "/" + name;

    ExpectApiSuccess(api.Create(name));


    Say() << "Check non-isolated hostname" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "/bin/sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "isolate", "false"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    AsRoot(api);
    ExpectEq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/bin/hostname"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", v));
    ExpectEq(v, GetHostname() + "\n");
    ExpectApiSuccess(api.Stop(name));

    RemakeDir(api, path);

    AsRoot(api);
    TMount tmpfs(name, path, "tmpfs", {"size=32m"});
    ExpectSuccess(tmpfs.Mount());
    AsNobody(api);

    AsRoot(api);
    BootstrapCommand("/bin/hostname", path, false);
    BootstrapCommand("/bin/sleep", path, false);
    BootstrapCommand("/bin/cat", path, false);
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty(name, "root", path));

    Say() << "Check default isolated hostname" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "isolate", "true"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    AsRoot(api);
    ExpectNeq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/hostname"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", v));
    ExpectEq(v, GetHostname() + "\n");
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check custom hostname" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "hostname", host));

    ExpectApiSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    AsRoot(api);
    ExpectNeq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/hostname"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", v));
    ExpectNeq(v, GetHostname() + "\n");
    ExpectEq(v, host + "\n");
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check /etc/hostname" << std::endl;
    AsRoot(api);
    ExpectApiSuccess(api.SetProperty(name, "virt_mode", "os"));
    AsNobody(api);

    TFolder d(path + "/etc");
    TFile f(path + "/etc/hostname");
    AsRoot(api);
    if (!d.Exists())
        ExpectSuccess(d.Create());
    ExpectSuccess(f.Touch());
    ExpectSuccess(f.GetPath().Chown(GetDefaultUser(), GetDefaultGroup()));
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/cat /etc/hostname"));
    ExpectApiSuccess(api.SetProperty(name, "stdout_path", path + "/stdout"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", v));
    ExpectNeq(v, GetHostname() + "\n");
    ExpectEq(v, host + "\n");

    AsRoot(api);
    ExpectSuccess(d.Remove(true));
    ExpectSuccess(tmpfs.Umount());
    AsNobody(api);

    ExpectApiSuccess(api.Destroy(name));
}

static void TestBindProperty(TPortoAPI &api) {
    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check bind parsing" << std::endl;
    ExpectApiFailure(api.SetProperty(name, "bind", "/tmp"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "bind", "qwerty /tmp"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "bind", "/tmp /bin"));
    ExpectApiFailure(api.SetProperty(name, "bind", "/tmp /bin xyz"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "bind", "/tmp /bin ro"));
    ExpectApiSuccess(api.SetProperty(name, "bind", "/tmp /bin rw"));
    ExpectApiFailure(api.SetProperty(name, "bind", "/tmp /bin ro; q"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "bind", "/tmp /bin ro; /tmp /sbin"));
    ExpectApiFailure(api.SetProperty(name, "bind", "/bin /sbin"), EError::InvalidValue);

    Say() << "Check bind without root isolation" << std::endl;
    string path = config().container().tmp_dir() + "/" + name;

    TFolder tmp("/tmp/27389");
    if (tmp.Exists())
        ExpectSuccess(tmp.Remove(true));
    ExpectSuccess(tmp.Create(0755, true));

    TFile f(tmp.GetPath() + "/porto");
    ExpectSuccess(f.Touch());

    ExpectApiSuccess(api.SetProperty(name, "command", "cat /proc/self/mountinfo"));
    ExpectApiSuccess(api.SetProperty(name, "bind", "/bin bin ro; /tmp/27389 tmp"));
    string v = StartWaitAndGetData(api, name, "stdout");
    auto m = ParseMountinfo(v);

    ExpectNeq(m[path + "/bin"].flags.find("ro,"), string::npos);
    ExpectNeq(m[path + "/tmp"].flags.find("rw,"), string::npos);
    ExpectApiSuccess(api.Stop(name));

    path = TMPDIR + "/" + name;

    RemakeDir(api, path);
    AsRoot(api);
    BootstrapCommand("/bin/cat", path, false);
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    ExpectApiSuccess(api.SetProperty(name, "root", path));
    ExpectApiSuccess(api.SetProperty(name, "bind", "/bin /bin ro; /tmp/27389 /tmp"));
    v = StartWaitAndGetData(api, name, "stdout");
    m = ParseMountinfo(v);
    ExpectNeq(m["/"].flags.find("rw,"), string::npos);
    ExpectNeq(m["/bin"].flags.find("ro,"), string::npos);
    ExpectNeq(m["/tmp"].flags.find("rw,"), string::npos);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure bind creates missing directories" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "bind", "/sbin /a/b/c ro; /sbin/init /x/y/z/init ro"));
    ExpectApiSuccess(api.Start(name));

    ExpectApiSuccess(api.Destroy(name));
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

        std::vector<std::string> flagsVec;
        ExpectSuccess(SplitString(flags, ',', flagsVec));

        bool up = std::find(flagsVec.begin(), flagsVec.end(), "UP") != flagsVec.end() ||
            std::find(flagsVec.begin(), flagsVec.end(), "UP>") != flagsVec.end();
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
    Say() << cmd << std::endl;
    vector<string> lines;
    ExpectSuccess(Popen(cmd, lines));
    ExpectEq(lines.size(), 1);
    return StringTrim(lines[0]);
}

static void TestXvlan(TPortoAPI &api, const std::string &name, const std::vector<std::string> &hostLink, const std::string &link, const std::string &type) {
    bool shouldShareMac = type == "ipvlan";
    ExpectApiSuccess(api.SetProperty(name, "command", "ip -o link show"));
    ExpectApiFailure(api.SetProperty(name, "net", type), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", type + " invalid " + link), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", type + " " + link), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "net", type + " " + link + " " + link));
    auto s = StartWaitAndGetData(api, name, "stdout");
    auto containerLink = StringToVec(s);
    ExpectEq(containerLink.size(),  2);
    Expect(containerLink != hostLink);
    ExpectEq(ShareMacAddress(hostLink, containerLink), shouldShareMac);
    auto linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    ExpectEq(linkMap.at("lo").up, true);
    Expect(linkMap.find(link) != linkMap.end());
    ExpectEq(linkMap.at(link).up, true);
    ExpectApiSuccess(api.Stop(name));

    if (type != "ipvlan") {
        string mtu = "1400";
        ExpectApiSuccess(api.SetProperty(name, "net", type + " " + link + " eth10 bridge " + mtu));
        s = StartWaitAndGetData(api, name, "stdout");
        containerLink = StringToVec(s);
        ExpectEq(containerLink.size(), 2);
        Expect(containerLink != hostLink);
        ExpectEq(ShareMacAddress(hostLink, containerLink), false);
        linkMap = IfHw(containerLink);
        Expect(linkMap.find("lo") != linkMap.end());
        ExpectEq(linkMap.at("lo").up, true);
        Expect(linkMap.find("eth10") != linkMap.end());
        ExpectEq(linkMap.at("eth10").mtu, mtu);
        ExpectEq(linkMap.at("eth10").up, true);
        ExpectApiSuccess(api.Stop(name));

        string hw = "00:11:22:33:44:55";
        ExpectApiSuccess(api.SetProperty(name, "net", type + " " + link + " eth10 bridge -1 " + hw));
        s = StartWaitAndGetData(api, name, "stdout");
        containerLink = StringToVec(s);
        ExpectEq(containerLink.size(), 2);
        Expect(containerLink != hostLink);
        ExpectEq(ShareMacAddress(hostLink, containerLink), false);
        linkMap = IfHw(containerLink);
        Expect(linkMap.find("lo") != linkMap.end());
        ExpectEq(linkMap.at("lo").up, true);
        Expect(linkMap.find("eth10") != linkMap.end());
        ExpectEq(linkMap.at("eth10").hw, hw);
        ExpectEq(linkMap.at("eth10").up, true);
        ExpectApiSuccess(api.Stop(name));
    }
}

static void CreateVethPair(TPortoAPI &api) {
    AsRoot(api);
    if (system("ip link | grep veth0") == 0) {
        Say() << "Delete link veth0" << std::endl;
        ExpectEq(system("ip link delete veth0"), 0);
    }
    if (system("ip link | grep veth1") == 0) {
        Say() << "Delete link veth1" << std::endl;
        int ret = system("ip link delete veth1");
        (void)ret;
    }
    ExpectEq(system("ip link add veth0 type veth peer name veth1"), 0);
    AsNobody(api);
}

static void TestNetProperty(TPortoAPI &api) {
    if (!NetworkEnabled()) {
        Say() << "Make sure network namespace is shared when network disabled" << std::endl;

        string pid;

        string name = "a";
        ExpectApiSuccess(api.Create(name));

        Say() << "Spawn long running task" << std::endl;
        ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectApiSuccess(api.Start(name));
        ExpectApiSuccess(api.GetData(name, "root_pid", pid));
        ExpectEq(TaskRunning(pid), true);

        AsRoot(api);
        ExpectEq(GetNamespace("self", "net"), GetNamespace(pid, "net"));

        ExpectApiSuccess(api.Destroy(name));

        return;
    }

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    ExpectApiFailure(api.SetProperty(name, "net_tos", "1"), EError::NotSupported);

    vector<string> hostLink;
    ExpectSuccess(Popen("ip -o link show", hostLink));

    string link = links[0]->GetAlias();

    Say() << "Check net parsing" << std::endl;
    ExpectApiFailure(api.SetProperty(name, "net", "qwerty"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", ""), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "net", "host"));
    ExpectApiSuccess(api.SetProperty(name, "net", "inherited"));
    ExpectApiSuccess(api.SetProperty(name, "net", "none"));
    ExpectApiFailure(api.SetProperty(name, "net", "host; macvlan " + link + " " + link), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", "host; host veth0"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", "host; host " + link), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "net", "host; host"));
    ExpectApiFailure(api.SetProperty(name, "net", "host; none"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", "host; inherited"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", "inherited; none"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", "inherited; macvlan " + link + " eth0"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "net", "none; macvlan " + link + " eth0"), EError::InvalidValue);

    Say() << "Check net=none" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "net", "none"));
    ExpectApiSuccess(api.SetProperty(name, "command", "ip -o link show"));
    string s = StartWaitAndGetData(api, name, "stdout");
    auto containerLink = StringToVec(s);
    ExpectEq(containerLink.size(), 1);
    Expect(containerLink != hostLink);
    ExpectEq(ShareMacAddress(hostLink, containerLink), false);
    auto linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    ExpectEq(linkMap.at("lo").up, true);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check net=host" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "net", "host"));
    s = StartWaitAndGetData(api, name, "stdout");
    containerLink = StringToVec(s);
    ExpectEq(containerLink.size(), hostLink.size());
    ExpectEq(ShareMacAddress(hostLink, containerLink), true);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    ExpectEq(linkMap.at("lo").up, true);
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "ip -o link show"));

    Say() << "Check net=host:veth0" << std::endl;

    CreateVethPair(api);

    ExpectApiFailure(api.SetProperty(name, "net", "host invalid"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "net", "host veth0"));
    s = StartWaitAndGetData(api, name, "stdout");
    containerLink = StringToVec(s);
    ExpectEq(containerLink.size(), 2);

    ExpectEq(ShareMacAddress(hostLink, containerLink), false);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    ExpectEq(linkMap.at("lo").up, true);
    Expect(linkMap.find("veth0") != linkMap.end());
    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure net=host:veth0 doesn't preserve L3 address" << std::endl;
    AsRoot(api);
    if (system("ip link | grep veth1") == 0) {
        Say() << "Delete link veth1" << std::endl;
        // we may race with kernel which removes dangling veth so don't
        // handle error
        int ret = system("ip link delete veth1");
        (void)ret;
    }
    ExpectEq(system("ip link"), 0);
    ExpectEq(system("ip link add veth0 type veth peer name veth1"), 0);
    ExpectEq(system("ip addr add dev veth0 1.2.3.4"), 0);
    AsNobody(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "ip -o -d addr show dev veth0 to 1.2.3.4"));
    ExpectApiSuccess(api.SetProperty(name, "net", "host veth0"));
    s = StartWaitAndGetData(api, name, "stdout");
    ExpectEq(s, "");
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check net=macvlan type" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "ip -o -d link show dev eth0"));
    ExpectApiSuccess(api.SetProperty(name, "net", "macvlan " + link + " eth0"));
    auto mode = StartWaitAndGetData(api, name, "stdout");
    ExpectNeq(mode.find("bridge"), std::string::npos);
    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.SetProperty(name, "net", "macvlan " + link + " eth0 passthru"));
    mode = StartWaitAndGetData(api, name, "stdout");
    ExpectNeq(mode.find("passthru"), std::string::npos);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check net=macvlan" << std::endl;
    TestXvlan(api, name, hostLink, link, "macvlan");

    Say() << "Check net=macvlan statistics" << std::endl;
    // create macvlan on default interface and ping ya.ru
    string uniq = "123";
    string gw = System("ip -o route | grep default | cut -d' ' -f3");
    string dev = System("ip -o route get " + gw + " | awk '{print $3}'");
    string addr = System("ip -o addr show " + dev + " | grep -w inet | awk '{print $4}'");
    string ip = System("echo " + addr + " | sed -e 's@\\([0-9]*\\.[0-9]*\\.[0-9]*\\.\\)[0-9]*\\(.*\\)@\\1" + uniq + "\\2@'");

    Say() << "Using device " << dev << " gateway " << gw << " ip " << addr << " -> " << ip << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "net", "macvlan " + dev + " " + dev));
    ExpectApiSuccess(api.SetProperty(name, "command", "false"));
    /* we now catch all packets (neighbor solicitation), not only ipv4, so can't expect 0 here
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "net_bytes[" + dev + "]", s));
    ExpectEq(s, "0");

    ExpectApiSuccess(api.Stop(name));
    */
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'ip addr add " + ip + " dev " + dev + " && ip route add default via " + gw + " && ping ya.ru -c 1 -w 1'"));
    AsRoot(api);
    ExpectApiSuccess(api.SetProperty(name, "user", "root"));
    ExpectApiSuccess(api.SetProperty(name, "group", "root"));

    ExpectApiSuccess(api.Start(name));
    AsNobody(api);
    WaitContainer(api, name, 60);
    ExpectApiSuccess(api.GetData(name, "net_bytes[" + dev + "]", s));
    ExpectNeq(s, "0");

    Say() << "Check net=veth" << std::endl;
    AsRoot(api);
    ExpectApiSuccess(api.Destroy(name));
    if (system("ip link | grep portobr0") == 0)
        ExpectEq(system("ip link delete portobr0"), 0);
    ExpectEq(system("ip link add portobr0 type bridge"), 0);
    ExpectEq(system("ip link set portobr0 up"), 0);
    AsNobody(api);

    ExpectApiSuccess(api.Create(name));
    ExpectApiFailure(api.SetProperty(name, "net", "veth eth0 invalid"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "net", "veth eth0 portobr0"));
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'sleep 1 && ip -o link show'"));

    vector<string> v;
    ExpectSuccess(Popen("ip -o link show", v));
    auto pre = IfHw(v);
    ExpectApiSuccess(api.Start(name));
    v.clear();
    ExpectSuccess(Popen("ip -o link show", v));
    auto post = IfHw(v);
    ExpectEq(pre.size() + 1, post.size());
    for (auto kv : pre)
        post.erase(kv.first);
    ExpectEq(post.size(), 1);
    auto portove = post.begin()->first;
    ExpectEq(post[portove].master, "portobr0");

    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "stdout", s));
    containerLink = StringToVec(s);
    ExpectEq(containerLink.size(), 2);
    Expect(containerLink != hostLink);
    ExpectEq(ShareMacAddress(hostLink, containerLink), false);
    linkMap = IfHw(containerLink);
    Expect(linkMap.find("lo") != linkMap.end());
    ExpectEq(linkMap.at("lo").up, true);
    Expect(linkMap.find("eth0") != linkMap.end());
    ExpectApiSuccess(api.Stop(name));

    v.clear();
    ExpectSuccess(Popen("ip -o link show", v));
    post = IfHw(v);
    Expect(post.find("portobr0") != post.end());
    AsRoot(api);
    ExpectEq(system("ip link delete portobr0"), 0);
    AsNobody(api);

    AsRoot(api);
    if (HaveIpVlan()) {
        AsNobody(api);
        Say() << "Check net=ipvlan" << std::endl;
        AsRoot(api);
        ExpectApiSuccess(api.SetProperty(name, "user", GetDefaultUser()));
        ExpectApiSuccess(api.SetProperty(name, "group", GetDefaultGroup()));
        AsNobody(api);
        TestXvlan(api, name, hostLink, link, "ipvlan");
    }
    AsNobody(api);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check net=host inheritance" << std::endl;
    std::string aPid, abPid;

    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a", "isolate", "true"));
    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "true"));

    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.GetData("a", "root_pid", aPid));
    ExpectApiSuccess(api.GetData("a/b", "root_pid", abPid));
    AsRoot(api);
    ExpectEq(GetNamespace(aPid, "net"), GetNamespace(abPid, "net"));
    ExpectEq(GetNamespace(aPid, "net"), GetNamespace("self", "net"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop("a"));

    CreateVethPair(api);

    ExpectApiSuccess(api.SetProperty("a", "net", "host veth0"));
    ExpectApiSuccess(api.SetProperty("a/b", "net", "inherited"));
    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.GetData("a", "root_pid", aPid));
    ExpectApiSuccess(api.GetData("a/b", "root_pid", abPid));
    AsRoot(api);
    ExpectEq(GetNamespace(aPid, "net"), GetNamespace(abPid, "net"));
    ExpectNeq(GetNamespace(aPid, "net"), GetNamespace("self", "net"));
    AsNobody(api);
    ExpectApiSuccess(api.Stop("a"));

    CreateVethPair(api);

    ExpectApiSuccess(api.SetProperty("a/b", "net", "none"));
    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.GetData("a", "root_pid", aPid));
    ExpectApiSuccess(api.GetData("a/b", "root_pid", abPid));
    AsRoot(api);
    ExpectNeq(GetNamespace(aPid, "net"), GetNamespace(abPid, "net"));
    ExpectNeq(GetNamespace(aPid, "net"), GetNamespace("self", "net"));
    AsNobody(api);
    ExpectApiSuccess(api.Destroy("a"));
}

static void TestAllowedDevicesProperty(TPortoAPI &api) {
    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Checking default allowed_devices" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectEq(GetCgKnob("devices", name, "devices.list"), "a *:* rwm");
    ExpectApiSuccess(api.Stop(name));

    Say() << "Checking custom allowed_devices" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "allowed_devices", "c 1:3 rwm; c 1:5 rwm"));
    ExpectApiSuccess(api.Start(name));
    ExpectEq(GetCgKnob("devices", name, "devices.list"), "c 1:3 rwm\nc 1:5 rwm");
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
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

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    Say() << "Make sure capabilities don't work for non-root container" << std::endl;

    ExpectApiFailure(api.SetProperty(name, "capabilities", "CHOWN"), EError::Permission);

    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectEq(GetCap(pid, "CapInh"), 0);
    ExpectEq(GetCap(pid, "CapPrm"), 0);
    ExpectEq(GetCap(pid, "CapEff"), 0);
    ExpectEq(GetCap(pid, "CapBnd"), defaultCap);
    ExpectApiSuccess(api.Stop(name));


    AsRoot(api);
    ExpectApiSuccess(api.SetProperty(name, "user", "root"));
    ExpectApiSuccess(api.SetProperty(name, "group", "root"));

    Say() << "Checking default capabilities" << std::endl;
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    ExpectEq(GetCap(pid, "CapInh"), defaultCap);
    ExpectEq(GetCap(pid, "CapPrm"), defaultCap);
    ExpectEq(GetCap(pid, "CapEff"), defaultCap);
    ExpectEq(GetCap(pid, "CapBnd"), defaultCap);

    ExpectApiSuccess(api.Stop(name));

    Say() << "Checking custom capabilities" << std::endl;
    ExpectApiFailure(api.SetProperty(name, "capabilities", "CHOWN; INVALID"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "capabilities", "CHOWN; DAC_OVERRIDE; FSETID; FOWNER; MKNOD; NET_RAW; SETGID; SETUID; SETFCAP; SETPCAP; NET_BIND_SERVICE; SYS_CHROOT; KILL; AUDIT_WRITE"));

    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    ExpectEq(GetCap(pid, "CapInh"), customCap);
    ExpectEq(GetCap(pid, "CapPrm"), customCap);
    ExpectEq(GetCap(pid, "CapEff"), customCap);
    ExpectEq(GetCap(pid, "CapBnd"), customCap);

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void CheckConnectivity(TPortoAPI &api, const std::string &name,
                              bool enabled, bool disabled) {
    string v;

    if (disabled) {
        ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
        ExpectApiSuccess(api.Start(name));
        WaitContainer(api, name);
        ExpectApiSuccess(api.GetData(name, "exit_status", v));
        ExpectNeq(v, "0");
        ExpectApiSuccess(api.Stop(name));
    }

    if (enabled) {
        ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));
        ExpectApiSuccess(api.Start(name));
        WaitContainer(api, name);
        ExpectApiSuccess(api.GetData(name, "exit_status", v));
        ExpectEq(v, "0");
        ExpectApiSuccess(api.Stop(name));
    }
}

static void TestEnablePortoProperty(TPortoAPI &api) {
    string name = "a";
    TPath path(TMPDIR + "/" + name);

    RemakeDir(api, path);
    AsRoot(api);
    BootstrapCommand("/usr/sbin/portotest", path.ToString());
    path.Chown("nobody", "nogroup");
    AsNobody(api);

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "/portotest connectivity"));

    Say() << "Non-isolated" << std::endl;

    ExpectApiFailure(api.SetProperty(name, "enable_porto", "false"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));

    Say() << "Root-isolated" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));

    Say() << "Namespace-isolated" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "root", "/"));
    ExpectApiSuccess(api.SetProperty(name, "porto_namespace", "a/"));
    ExpectApiFailure(api.SetProperty(name, "enable_porto", "false"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));

    Say() << "Isolated" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));

    CheckConnectivity(api, name, true, true);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Isolated hierarchy" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("a/b"));

    ExpectApiSuccess(api.SetProperty("a/b", "command", "/portotest connectivity"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "true"));
    ExpectApiSuccess(api.SetProperty("a/b", "porto_namespace", "a/"));
    ExpectApiSuccess(api.SetProperty("a/b", "root", path.ToString()));

    CheckConnectivity(api, "a/b", true, true);

    ExpectApiSuccess(api.Stop("a"));
    ExpectApiSuccess(api.SetProperty("a/b", "root", "/"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("a/b", "porto_namespace", ""));
    ExpectApiSuccess(api.SetProperty("a", "porto_namespace", "a/"));
    ExpectApiSuccess(api.SetProperty("a", "root", path.ToString()));

    CheckConnectivity(api, "a/b", true, false);

    ExpectApiSuccess(api.Destroy("a"));
}

static void TestStateMachine(TPortoAPI &api) {
    string name = "a";
    string pid;
    string v;

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "stopped");

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "running");

    ExpectApiFailure(api.Start(name), EError::InvalidState);

    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    WaitProcessExit(pid);
    ExpectApiSuccess(api.GetData(name, "state", v));
    Expect(v == "running" || v == "dead");

    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "dead");

    ExpectApiFailure(api.Start(name), EError::InvalidState);

    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "stopped");

    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "stopped");

    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'while :; do :; done'"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    v = GetState(pid);
    ExpectEq(v, "R");

    ExpectApiSuccess(api.Pause(name));
    v = GetState(pid);
    ExpectEq(v, "D");

    ExpectApiFailure(api.Pause(name), EError::InvalidState);

    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "paused");

    ExpectApiSuccess(api.Resume(name));
    v = GetState(pid);
    ExpectEq(v, "R");

    ExpectApiFailure(api.Resume(name), EError::InvalidState);

    ExpectApiSuccess(api.Stop(name));
    WaitProcessExit(pid);

    Say() << "Make sure we can stop unintentionally frozen container " << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    v = GetFreezer(name);
    ExpectEq(v, "THAWED\n");

    AsRoot(api);
    SetFreezer(name, "FROZEN");
    AsNobody(api);

    v = GetFreezer(name);
    ExpectEq(v, "FROZEN\n");

    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure we can remove paused container " << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Pause(name));
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure kill SIGTERM works" << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);
    ExpectApiSuccess(api.Kill(name, SIGTERM));
    WaitContainer(api, name);
    ExpectEq(TaskRunning(pid), false);
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "dead");
    ExpectApiSuccess(api.GetData(name, "exit_status", v));
    ExpectEq(v, string("15"));
    ExpectApiSuccess(api.Destroy(name));

    // if container init process doesn't have custom handler for a signal
    // it's ignored
    Say() << "Make sure init in container ignores SIGTERM but dies after SIGKILL" << std::endl;
    ExpectApiSuccess(api.Create(name));
    AsRoot(api);
    ExpectApiSuccess(api.SetProperty(name, "virt_mode", "os"));
    AsNobody(api);
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);
    ExpectApiSuccess(api.Kill(name, SIGTERM));
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "running");
    ExpectEq(TaskRunning(pid), true);
    ExpectApiSuccess(api.Kill(name, SIGKILL));
    WaitContainer(api, name);
    ExpectEq(TaskRunning(pid), false);
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "dead");
    ExpectApiSuccess(api.GetData(name, "exit_status", v));
    ExpectEq(v, string("9"));

    // we can't kill root or non-running container
    ExpectApiFailure(api.Kill(name, SIGKILL), EError::InvalidState);
    ExpectApiFailure(api.Kill("/", SIGKILL), EError::Permission);

    ExpectApiSuccess(api.Destroy(name));
}

static void TestPath(TPortoAPI &api) {
    vector<pair<string, string>> normalize = {
        { "",   "" },
        { ".",  "." },
        { "..", ".." },
        { "a",  "a" },
        { "/a",   "/a" },
        { "/a/b/c",   "/a/b/c" },
        { "////a//",   "/a" },
        { "/././.",   "/" },
        { "/a/..",   "/" },
        { "a/..",   "." },
        { "../a/../..",   "../.." },
        { "/a/../..",   "/" },
        { "/abc/cde/../..",   "/" },
        { "/abc/../cde/.././../abc",   "/abc" },

        /* Stolen from golang src/path/filepath/path_test.go */

        // Already clean
        {"abc", "abc"},
        {"abc/def", "abc/def"},
        {"a/b/c", "a/b/c"},
        {".", "."},
        {"..", ".."},
        {"../..", "../.."},
        {"../../abc", "../../abc"},
        {"/abc", "/abc"},
        {"/", "/"},

        // Remove trailing slash
        {"abc/", "abc"},
        {"abc/def/", "abc/def"},
        {"a/b/c/", "a/b/c"},
        {"./", "."},
        {"../", ".."},
        {"../../", "../.."},
        {"/abc/", "/abc"},

        // Remove doubled slash
        {"abc//def//ghi", "abc/def/ghi"},
        {"//abc", "/abc"},
        {"///abc", "/abc"},
        {"//abc//", "/abc"},
        {"abc//", "abc"},

        // Remove . elements
        {"abc/./def", "abc/def"},
        {"/./abc/def", "/abc/def"},
        {"abc/.", "abc"},

        // Remove .. elements
        {"abc/def/ghi/../jkl", "abc/def/jkl"},
        {"abc/def/../ghi/../jkl", "abc/jkl"},
        {"abc/def/..", "abc"},
        {"abc/def/../..", "."},
        {"/abc/def/../..", "/"},
        {"abc/def/../../..", ".."},
        {"/abc/def/../../..", "/"},
        {"abc/def/../../../ghi/jkl/../../../mno", "../../mno"},
        {"/../abc", "/abc"},

        // Combinations
        {"abc/./../def", "def"},
        {"abc//./../def", "def"},
        {"abc/../../././../def", "../../def"},
    };

    vector<vector<string>> inner = {
        { "/", "/", ".", "/" },
        { "/", "a", "", "" },
        { "a", "/", "", "" },
        { "/", "", "", "" },
        { "", "/", "", "" },
        { "/", "/abc", "abc", "/abc" },
        { "/", "/abc/def", "abc/def", "/abc/def" },
        { "/abc", "/abc", ".", "/" },
        { "/abc", "/abc/def", "def", "/def" },
        { "/abc", "/abcdef", "", "" },
        { "/abcdef", "/abc", "", "" },
        { "/abc/def", "/abc", "", "" },
        { "abc", "abc", ".", "/" },
        { "abc", "abc/def", "def", "/def" },
        { "abc", "abcdef", "", "" },
    };

    for (auto n: normalize)
        ExpectEq(TPath(n.first).NormalPath().ToString(), n.second);

    for (auto n: inner) {
        ExpectEq(TPath(n[0]).InnerPath(n[1], false).ToString(), n[2]);
        ExpectEq(TPath(n[0]).InnerPath(n[1], true).ToString(), n[3]);
        if (n[3] != "")
            ExpectEq((TPath(n[0]) / n[3]).ToString(), n[1]);
    }
}

static void TestIdmap(TPortoAPI &api) {
    TIdMap idmap;
    uint16_t id;

    for (uint16_t i = 1; i < 256; i++) {
        ExpectSuccess(idmap.Get(id));
        ExpectEq(id, i);
    }

    for (uint16_t i = 1; i < 256; i++)
        idmap.Put(i);

    ExpectSuccess(idmap.Get(id));
    ExpectEq(id, 1);

    uint16_t id1, id2;
    ExpectSuccess(idmap.GetSince(5000, id1));
    ExpectSuccess(idmap.GetSince(5000, id2));
    Expect(id1 > 5000);
    Expect(id2 > 5000);
    ExpectNeq(id1, id2);
}

static void TestRoot(TPortoAPI &api) {
    string v;
    string root = "/";
    string porto_root = "/porto";
    vector<string> properties = {
        "command",
        "user",
        "group",
        "env",
        "cwd",
        "memory_limit",
        "cpu_policy",
        "cpu_limit",
        "cpu_guarantee",
        "io_policy",
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
        "root_readonly",
        "virt_mode",
        "aging_time",
        "porto_namespace",
        "enable_porto",
    };

    if (HaveLowLimit())
        properties.push_back("memory_guarantee");

    if (HaveRechargeOnPgfault())
        properties.push_back("recharge_on_pgfault");

    if (HaveIoLimit())
        properties.push_back("io_limit");

    if (NetworkEnabled()) {
        properties.push_back("net");
        /*
        properties.push_back("net_tos");
        */
        properties.push_back("net_guarantee");
        properties.push_back("net_limit");
        properties.push_back("net_priority");
    }

    vector<string> data = {
        "absolute_name",
        "state",
        "oom_killed",
        "respawn_count",
        "exit_status",
        "start_errno",
        "stdout",
        "stderr",
        "cpu_usage",
        "memory_usage",
        "minor_faults",
        "major_faults",
        "io_read",
        "io_write",
        "time",
    };

    if (NetworkEnabled()) {
        data.push_back("net_bytes");
        data.push_back("net_packets");
        data.push_back("net_drops");
        data.push_back("net_overlimits");
    }

    if (HaveMaxRss())
        data.push_back("max_rss");

    std::vector<TProperty> plist;

    ExpectApiSuccess(api.Plist(plist));
    ExpectEq(plist.size(), properties.size());

    for (auto p: plist)
        Expect(std::find(properties.begin(), properties.end(), p.Name) != properties.end());

    std::vector<TData> dlist;

    ExpectApiSuccess(api.Dlist(dlist));
    ExpectEq(dlist.size(), data.size());

    for (auto d: dlist)
        Expect(std::find(data.begin(), data.end(), d.Name) != data.end());

    Say() << "Check root cpu_usage & memory_usage" << std::endl;
    ExpectApiSuccess(api.GetData(porto_root, "cpu_usage", v));
    ExpectEq(v, "0");
    ExpectApiSuccess(api.GetData(porto_root, "memory_usage", v));
    ExpectEq(v, "0");

    for (auto &link : links) {
        ExpectApiSuccess(api.GetData(porto_root, "net_bytes[" + link->GetAlias() + "]", v));
        ExpectEq(v, "0");
        ExpectApiSuccess(api.GetData(porto_root, "net_packets[" + link->GetAlias() + "]", v));
        ExpectEq(v, "0");
        ExpectApiSuccess(api.GetData(porto_root, "net_drops[" + link->GetAlias() + "]", v));
        ExpectEq(v, "0");
        ExpectApiSuccess(api.GetData(porto_root, "net_overlimits[" + link->GetAlias() + "]", v));
        ExpectEq(v, "0");
    }

    if (IsCfqActive()) {
        ExpectApiSuccess(api.GetData(porto_root, "io_read", v));
        ExpectEq(v, "");
        ExpectApiSuccess(api.GetData(porto_root, "io_write", v));
        ExpectEq(v, "");
    }

    if (NetworkEnabled()) {
        uint32_t defClass = TcHandle(1, 2);
        uint32_t rootClass = TcHandle(1, 1);
        uint32_t portoRootClass = TcHandle(1, 3);
        uint32_t nextClass = TcHandle(1, 4);

        uint32_t rootQdisc = TcHandle(1, 0);
        uint32_t nextQdisc = TcHandle(2, 0);

        ExpectEq(TcQdiscExist(rootQdisc), true);
        ExpectEq(TcQdiscExist(nextQdisc), false);
        ExpectEq(TcClassExist(defClass), true);
        ExpectEq(TcClassExist(rootClass), true);
        ExpectEq(TcClassExist(portoRootClass), true);
        ExpectEq(TcClassExist(nextClass), false);
        ExpectEq(TcCgFilterExist(rootQdisc, 1), true);
        ExpectEq(TcCgFilterExist(rootQdisc, 2), false);
    }

    Say() << "Check root properties & data" << std::endl;
    for (auto p : properties)
        ExpectApiFailure(api.GetProperty(root, p, v), EError::InvalidProperty);

    ExpectApiSuccess(api.GetData(root, "state", v));
    ExpectEq(v, string("meta"));
    ExpectApiFailure(api.GetData(root, "exit_status", v), EError::InvalidState);
    ExpectApiFailure(api.GetData(root, "start_errno", v), EError::InvalidState);
    ExpectApiSuccess(api.GetData(root, "root_pid", v));
    ExpectApiFailure(api.GetData(root, "stdout", v), EError::InvalidState);
    ExpectApiSuccess(api.GetData(root, "parent", v));
    ExpectEq(v, "");
    ExpectApiFailure(api.GetData(root, "stderr", v), EError::InvalidState);
    ExpectApiSuccess(api.GetData(root, "time", v));

    Say() << "Check that stop on root stops all children" << std::endl;

    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("b"));
    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("b", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("a"));
    ExpectApiSuccess(api.Start("b"));

    ExpectApiFailure(api.Destroy(root), EError::Permission);
    ExpectApiSuccess(api.Destroy("a"));
    ExpectApiSuccess(api.Destroy("b"));

    Say() << "Check cpu_limit/cpu_guarantee" << std::endl;
    if (HaveCfsBandwidth())
        ExpectEq(GetCgKnob("cpu", "", "cpu.cfs_quota_us"), "-1");
    if (HaveCfsGroupSched())
        ExpectEq(GetCgKnob("cpu", "", "cpu.shares"), "1024");
    if (IsCfqActive())
        ExpectEq(GetCgKnob("blkio", "", "blkio.weight"), "1000");
}

static void TestDataMap(TPortoAPI &api, const std::string &name, const std::string &data) {
    std::string full;
    vector<string> lines;

    ExpectApiSuccess(api.GetData(name, data, full));
    ExpectNeq(full, "");
    ExpectSuccess(SplitString(full, ';', lines));

    ExpectNeq(lines.size(), 0);
    for (auto line: lines) {
        string tmp;
        vector<string> tokens;

        ExpectSuccess(SplitString(line, ':', tokens));
        ExpectApiSuccess(api.GetData(name, data + "[" + StringTrim(tokens[0]) + "]", tmp));
        ExpectEq(tmp, StringTrim(tokens[1]));
    }

    ExpectApiFailure(api.GetData(name, data + "[invalid]", full), EError::InvalidValue);
}

static void ExpectNonZeroLink(TPortoAPI &api, const std::string &name,
                              const std::string &data) {
#if 1
    int nonzero = 0;

    for (auto &link : links) {
        string v;
        ExpectApiSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));

        if (v != "0" && v != "-1")
            nonzero++;
    }
    ExpectNeq(nonzero, 0);
#else
    for (auto &link : links) {
        string v;
        ExpectApiSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));
        Expect(v != "0" && v != "-1");
    }
#endif
}

static void ExpectRootLink(TPortoAPI &api, const std::string &name,
                           const std::string &data) {
    for (auto &link : links) {
        string v, rv;
        ExpectApiSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));
        ExpectApiSuccess(api.GetData("/", data + "[" + link->GetAlias() + "]", rv));
        ExpectEq(v, rv);
    }
}

static void ExpectZeroLink(TPortoAPI &api, const std::string &name,
                           const std::string &data) {
    for (auto &link : links) {
        string v;
        ExpectApiSuccess(api.GetData(name, data + "[" + link->GetAlias() + "]", v));
        ExpectEq(v, "0");
    }
}

static void TestData(TPortoAPI &api) {
    // should be executed right after TestRoot because assumes empty statistics

    string root = "/";
    string wget = "wget";
    string noop = "noop";

    ExpectApiSuccess(api.Create(noop));
    // this will cause io read and noop will not have io_read
    ExpectEq(system("ls -la /bin >/dev/null"), 0);
    ExpectApiSuccess(api.SetProperty(noop, "command", "ls -la /bin"));
    ExpectApiSuccess(api.Start(noop));
    WaitContainer(api, noop);

    ExpectApiSuccess(api.Create(wget));
    if (NetworkEnabled())
        ExpectApiSuccess(api.SetProperty(wget, "command", "bash -c 'wget yandex.ru && sync'"));
    else
        ExpectApiSuccess(api.SetProperty(wget, "command", "bash -c 'dd if=/dev/urandom bs=4M count=1 of=/tmp/porto.tmp && sync'"));
    ExpectApiSuccess(api.Start(wget));
    WaitContainer(api, wget, 60);

    string v, rv;
    ExpectApiSuccess(api.GetData(wget, "exit_status", v));
    ExpectEq(v, string("0"));
    ExpectApiSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectApiSuccess(api.GetData(root, "memory_usage", v));
    Expect(v != "0" && v != "-1");

    if (IsCfqActive()) {
        Say() << "Make sure io_write counters are valid" << std::endl;
        ExpectApiSuccess(api.GetData(root, "io_write", v));
        ExpectNeq(v, "");
        TestDataMap(api, root, "io_write");

        Say() << "Make sure io_read counters are valid" << std::endl;
        ExpectApiSuccess(api.GetData(root, "io_read", v));
        Expect(v != "");
        TestDataMap(api, root, "io_read");
    }
    ExpectApiSuccess(api.GetData(wget, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectApiSuccess(api.GetData(wget, "memory_usage", v));
    Expect(v != "0" && v != "-1");
    if (IsCfqActive()) {
        ExpectApiSuccess(api.GetData(wget, "io_write", v));
        ExpectNeq(v, "");
        ExpectApiSuccess(api.GetData(wget, "io_read", v));
        ExpectNeq(v, "");
    }

    ExpectApiSuccess(api.GetData(noop, "cpu_usage", v));
    Expect(v != "0" && v != "-1");
    ExpectApiSuccess(api.GetData(noop, "memory_usage", v));
    Expect(v != "0" && v != "-1");
    if (IsCfqActive()) {
        ExpectApiSuccess(api.GetData(noop, "io_write", v));
        ExpectEq(v, "");
        ExpectApiSuccess(api.GetData(noop, "io_read", v));
        ExpectEq(v, "");
    }

    if (NetworkEnabled()) {
        Say() << "Make sure net_bytes counters are valid" << std::endl;
        ExpectNonZeroLink(api, root, "net_bytes");
        ExpectRootLink(api, wget, "net_bytes");
        ExpectZeroLink(api, noop, "net_bytes");

        Say() << "Make sure net_packets counters are valid" << std::endl;
        ExpectNonZeroLink(api, root, "net_packets");
        ExpectRootLink(api, wget, "net_packets");
        ExpectZeroLink(api, noop, "net_packets");

        Say() << "Make sure net_drops counters are valid" << std::endl;
        ExpectZeroLink(api, root, "net_drops");
        ExpectZeroLink(api, wget, "net_drops");
        ExpectZeroLink(api, noop, "net_drops");

        Say() << "Make sure net_overlimits counters are valid" << std::endl;
        ExpectZeroLink(api, root, "net_overlimits");
        ExpectZeroLink(api, wget, "net_overlimits");
        ExpectZeroLink(api, noop, "net_overlimits");
    }

    ExpectApiSuccess(api.Destroy(wget));
    ExpectApiSuccess(api.Destroy(noop));
}

static bool CanTestLimits() {
    if (!HaveLowLimit())
        return false;

    if (!HaveRechargeOnPgfault())
        return false;

    if (!HaveSmart())
        return false;

    return true;
}

static TUintMap ParseMap(const std::string &s) {
    TUintMap m;
    std::vector<std::string> lines;
    TError error = SplitEscapedString(s, ';', lines);
    for (auto &line : lines) {
        std::vector<std::string> nameval;

        ExpectSuccess(SplitEscapedString(line, ':', nameval));
        ExpectEq(nameval.size(), 2);

        std::string key = StringTrim(nameval[0]);
        uint64_t val;

        ExpectSuccess(StringToUint64(nameval[1], val));

        m[key] = val;
    }

    return m;
}

static void TestCoresConvertion(TPortoAPI &api, const std::string &name, const std::string &property) {
    auto cores = GetNumCores();
    std::string v;

    ExpectApiSuccess(api.SetProperty(name, property, std::to_string(cores) + "c"));
    ExpectApiSuccess(api.GetProperty(name, property, v));
    ExpectEq(v, "100");

    ExpectApiSuccess(api.SetProperty(name, property, std::to_string(cores / 2) + "c"));
    ExpectApiSuccess(api.GetProperty(name, property, v));
    ExpectEq(v, "50");
}

static void TestLimits(TPortoAPI &api) {
    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check default limits" << std::endl;
    string current;

    current = GetCgKnob("memory", "", "memory.use_hierarchy");
    ExpectEq(current, "1");

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.use_hierarchy");
    ExpectEq(current, "1");

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX));

    if (HaveLowLimit()) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        ExpectEq(current, "0");
    }
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check custom limits" << std::endl;
    string exp_limit = "134217728";
    string exp_guar = "16384";
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "1g"));
    ExpectApiSuccess(api.GetProperty(name, "memory_limit", current));
    ExpectEq(current, "1073741824");

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    if (HaveLowLimit())
        ExpectApiSuccess(api.SetProperty(name, "memory_guarantee", exp_guar));
    ExpectApiSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);
    if (HaveLowLimit()) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        ExpectEq(current, exp_guar);
    }

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "2g"));
    ExpectApiFailure(api.SetProperty(name, "memory_limit", "10k"), EError::InvalidValue);

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "0"));

    Say() << "Check cpu_limit and cpu_guarantee range" << std::endl;
    if (HaveCfsBandwidth()) {
        ExpectApiFailure(api.SetProperty(name, "cpu_limit", "test"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "cpu_limit", "0"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "cpu_limit", "101"), EError::InvalidValue);
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "1"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "100"));
    }

    if (HaveCfsGroupSched()) {
        ExpectApiFailure(api.SetProperty(name, "cpu_guarantee", "test"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "cpu_guarantee", "-1"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "cpu_guarantee", "101"), EError::InvalidValue);
        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "0"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "100"));
    }

    Say() << "Check cpu_policy" << std::endl;
    string smart;

    ExpectApiFailure(api.SetProperty(name, "cpu_policy", "somecrap"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "cpu_policy", "idle"), EError::NotSupported);

    if (HaveSmart()) {
        ExpectApiSuccess(api.SetProperty(name, "cpu_policy", "rt"));
        ExpectApiSuccess(api.Start(name));
        smart = GetCgKnob("cpu", name, "cpu.smart");
        ExpectEq(smart, "1");
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_policy", "normal"));
        ExpectApiSuccess(api.Start(name));
        smart = GetCgKnob("cpu", name, "cpu.smart");
        ExpectEq(smart, "0");
        ExpectApiSuccess(api.Stop(name));
    }

    if (HaveCfsBandwidth()) {
        Say() << "Check cpu_limit" << std::endl;
        ExpectApiSuccess(api.SetProperty(name, "cpu_policy", "normal"));

        uint64_t period, quota;
        ExpectSuccess(StringToUint64(GetCgKnob("cpu", "", "cpu.cfs_period_us"), period));
        long ncores = sysconf(_SC_NPROCESSORS_CONF);

        const uint64_t minQuota = 1 * 1000;
        uint64_t half = ncores * period / 2;
        if (half < minQuota)
            half = minQuota;

        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "20"));
        ExpectApiSuccess(api.Start(name));
        ExpectSuccess(StringToUint64(GetCgKnob("cpu", name, "cpu.cfs_quota_us"), quota));
        Say() << "quota=" << quota << " half="<< half << " min=" << minQuota << std::endl;

        Expect(quota < half);
        Expect(quota > minQuota);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "80"));
        ExpectApiSuccess(api.Start(name));
        ExpectSuccess(StringToUint64(GetCgKnob("cpu", name, "cpu.cfs_quota_us"), quota));
        Say() << "quota=" << quota << " half="<< half << " min=" << minQuota << std::endl;
        Expect(quota > half);
        Expect(quota > minQuota);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "100"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("cpu", name, "cpu.cfs_quota_us"), "-1");
        ExpectApiSuccess(api.Stop(name));

        TestCoresConvertion(api, name, "cpu_limit");
    }

    if (HaveCfsGroupSched()) {
        Say() << "Check cpu_guarantee" << std::endl;
        uint64_t rootShares, shares;
        ExpectSuccess(StringToUint64(GetCgKnob("cpu", "", "cpu.shares"), rootShares));

        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "0"));
        ExpectApiSuccess(api.Start(name));
        ExpectSuccess(StringToUint64(GetCgKnob("cpu", name, "cpu.shares"), shares));
        ExpectEq(shares, rootShares);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "1"));
        ExpectApiSuccess(api.Start(name));
        ExpectSuccess(StringToUint64(GetCgKnob("cpu", name, "cpu.shares"), shares));
        ExpectEq(shares, rootShares);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "100"));
        ExpectApiSuccess(api.Start(name));
        ExpectSuccess(StringToUint64(GetCgKnob("cpu", name, "cpu.shares"), shares));
        ExpectEq(shares, rootShares * 100);
        ExpectApiSuccess(api.Stop(name));

        TestCoresConvertion(api, name, "cpu_guarantee");
    }

    if (IsCfqActive()) {
        Say() << "Check io_policy" << std::endl;
        uint64_t rootWeight, weight;
        ExpectSuccess(StringToUint64(GetCgKnob("blkio", "", "blkio.weight"), rootWeight));

        ExpectApiFailure(api.SetProperty(name, "io_policy", "invalid"), EError::InvalidValue);

        ExpectApiSuccess(api.SetProperty(name, "io_policy", "normal"));
        ExpectApiSuccess(api.Start(name));
        ExpectSuccess(StringToUint64(GetCgKnob("blkio", name, "blkio.weight"), weight));
        ExpectEq(weight, rootWeight);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "io_policy", "batch"));
        ExpectApiSuccess(api.Start(name));
        ExpectSuccess(StringToUint64(GetCgKnob("blkio", name, "blkio.weight"), weight));
        Expect(weight != rootWeight || weight == config().container().batch_io_weight());
        ExpectApiSuccess(api.Stop(name));
    }

    if (HaveIoLimit()) {
        Say() << "Check io_limit" << std::endl;

        ExpectApiSuccess(api.SetProperty(name, "io_limit", "0"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("memory", name, "memory.fs_bps_limit"), "0");
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "io_limit", "1000"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("memory", name, "memory.fs_bps_limit"), "1000");
        ExpectApiSuccess(api.Stop(name));
    }

    Say() << "Check net_cls cgroup" << std::endl;

    uint32_t netGuarantee = 100000, netCeil = 200000, netPrio = 4;

    uint32_t i = 0;
    for (auto &link : links) {
        ExpectApiSuccess(api.SetProperty(name, "net_guarantee[" + link->GetAlias() + "]", std::to_string(netGuarantee + i)));
        ExpectApiSuccess(api.SetProperty(name, "net_limit[" + link->GetAlias() + "]", std::to_string(netCeil + i)));
        ExpectApiFailure(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", "-1"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", "8"), EError::InvalidValue);
        ExpectApiSuccess(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", "0"));
        ExpectApiSuccess(api.SetProperty(name, "net_priority[" + link->GetAlias() + "]", std::to_string(netPrio + i)));
        i++;
    }
    ExpectApiSuccess(api.Start(name));

    if (NetworkEnabled()) {
        string handle = GetCgKnob("net_cls", name, "net_cls.classid");

        i = 0;
        for (auto &link : links) {
            ExpectSuccess(link->RefillClassCache());
            uint32_t prio, rate, ceil;
            TNlClass tclass(link, -1, stoul(handle));
            ExpectSuccess(tclass.GetProperties(prio, rate, ceil));
            ExpectEq(prio, netPrio + i);
            ExpectEq(rate, netGuarantee + i);
            ExpectEq(ceil, netCeil + i);

            i++;
        }

        ExpectApiSuccess(api.Stop(name));

        Say() << "Make sure we can set map properties without subscript" << std::endl;

        std::string guarantee, v;
        ExpectApiSuccess(api.GetProperty(name, "net_guarantee", guarantee));

        auto m = ParseMap(guarantee);

        guarantee = "";
        for (auto pair : m)
            guarantee += pair.first + ": 1000; ";
        ExpectNeq(guarantee.length(), 0);
        ExpectApiSuccess(api.SetProperty(name, "net_guarantee", guarantee));
        ExpectApiSuccess(api.GetProperty(name, "net_guarantee", v));
        ExpectEq(StringTrim(guarantee, " ;"), v);

        Say() << "Make sure we have a cap for stdout_limit property" << std::endl;

        ExpectApiFailure(api.SetProperty(name, "stdout_limit", std::to_string(config().container().stdout_limit() + 1)), EError::InvalidValue);

        Say() << "Make sure we have a cap for private property" << std::endl;
        std::string tooLong = std::string(config().container().private_max() + 1, 'a');
        ExpectApiFailure(api.SetProperty(name, "stdout_limit", tooLong), EError::InvalidValue);
    }

    ExpectApiSuccess(api.Destroy(name));
}

static void TestUlimitProperty(TPortoAPI &api) {
    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check rlimits parsing" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "ulimit", ""));
    ExpectApiFailure(api.SetProperty(name, "ulimit", "qwe"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "qwe: 123"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "qwe: 123 456"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "as: 123"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "as 123 456"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "as: 123 456 789"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "as: 123 :456"), EError::InvalidValue);

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

    ExpectApiSuccess(api.SetProperty(name, "ulimit", ulimit));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    string pid;
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    AsRoot(api);

    for (auto &lim : rlim) {
        ExpectEq(GetRlimit(pid, lim.first, true), lim.second.first);
        ExpectEq(GetRlimit(pid, lim.first, false), lim.second.second);
    }

    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure we can set limit to unlimited" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "ulimit", "data: unlim unlimited"));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestVirtModeProperty(TPortoAPI &api) {
    std::string name = "lxc";

    Say() << "Check permissions " << std::endl;

    ExpectApiSuccess(api.Create(name));
    ExpectApiFailure(api.SetProperty(name, "virt_mode", "os"), EError::Permission);
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check that we can't start without loop" << std::endl;

    std::map<std::string, std::string> expected = {
        { "command", "/sbin/init" },
        { "stdin_path", "/dev/null" },
        { "stdout_path", "/dev/null" },
        { "stderr_path", "/dev/null" },
        { "net", "none" },
        { "isolate", "true" },
        { "bind_dns", "false" },
        { "bind", "" },
        { "cwd", "/" },
        { "allowed_devices", "c 1:3 rwm; c 1:5 rwm; c 1:7 rwm; c 1:9 rwm; c 1:8 rwm; c 136:* rw; c 5:2 rwm; c 254:0 rm; c 254:0 rm; c 10:237 rmw; b 7:* rmw" },
        { "capabilities", "CHOWN; DAC_OVERRIDE; FOWNER; FSETID; IPC_LOCK; KILL; NET_ADMIN; NET_BIND_SERVICE; NET_RAW; SETGID; SETUID; SYS_CHROOT; SYS_RESOURCE" },
    };
    std::string s;

    AsDaemon(api);
    ExpectApiSuccess(api.Create(name));
    ExpectApiFailure(api.SetProperty(name, "virt_mode", "invalid"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "virt_mode", "os"));

    for (auto kv : expected) {
        ExpectApiSuccess(api.GetProperty(name, kv.first, s));
        ExpectEq(s, kv.second);
    }

    ExpectApiFailure(api.SetProperty(name, "root", "/tmp"), EError::Permission);

    Say() << "Check credentials and default roolback" << std::endl;

    TPath tmpdir = "/tmp/portotest.dir";
    TPath tmpimg = "/tmp/portotest.img";

    std::string cmd = std::string("dd if=/dev/zero of=") + tmpimg.ToString() + " bs=1 count=1 seek=128M && mkfs.ext4 -F -F " + tmpimg.ToString();

    ExpectEq(system(cmd.c_str()), 0);

    TFolder dir(tmpdir, true);
    dir.Remove(true);
    ExpectSuccess(dir.Create(0755, false));

    int nr;
    AsRoot(api);
    TError error = SetupLoopDevice(tmpimg, nr);
    if (error)
        throw error.GetMsg();
    TMount m("/dev/loop" + std::to_string(nr), tmpdir, "ext4", {});
    AsDaemon(api);

    try {
        AsRoot(api);
        ExpectSuccess(m.Mount());
        AsDaemon(api);

        ExpectApiSuccess(api.SetProperty(name, "isolate", "false"));
        ExpectApiSuccess(api.SetProperty(name, "root", tmpimg.ToString()));

        AsRoot(api);
        BootstrapCommand("/usr/bin/id", tmpdir.ToString());
        cmd = std::string("mkdir ") + tmpdir.ToString() + "/sbin";
        ExpectEq(system(cmd.c_str()), 0);
        cmd = std::string("mv ") + tmpdir.ToString() + "/id " + tmpdir.ToString() + "/sbin/init";
        ExpectEq(system(cmd.c_str()), 0);
        (void)m.Umount();
        (void)PutLoopDev(nr);
        AsDaemon(api);
    } catch (...) {
        AsRoot(api);
        (void)m.Umount();
        (void)PutLoopDev(nr);
        ExpectApiSuccess(api.Destroy(name));
        throw;
    }

    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);

    for (auto kv : expected) {
        ExpectApiSuccess(api.GetProperty(name, kv.first, s));
        ExpectEq(s, kv.second);
    }
    ExpectApiSuccess(api.Destroy(name));
}

static void TestAlias(TPortoAPI &api) {
    if (!HaveLowLimit())
        return;
    if (!HaveRechargeOnPgfault())
        return;
    if (!HaveSmart())
        return;

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check default limits" << std::endl;
    string current;
    string alias, real;

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectApiSuccess(api.GetProperty(name, "memory_limit", real));
    ExpectEq(alias, real);
    ExpectApiSuccess(api.GetProperty(name, "memory.low_limit_in_bytes", alias));
    ExpectApiSuccess(api.GetProperty(name, "memory_guarantee", real));
    ExpectEq(alias, real);
    ExpectApiSuccess(api.GetProperty(name, "memory.recharge_on_pgfault", alias));
    ExpectApiSuccess(api.GetProperty(name, "recharge_on_pgfault", real));
    ExpectEq(alias, "0");
    ExpectEq(real, "false");
    ExpectApiSuccess(api.GetProperty(name, "cpu.smart", alias));
    ExpectApiSuccess(api.GetProperty(name, "cpu_policy", real));
    ExpectEq(alias, "0");
    ExpectEq(real, "normal");
    ExpectApiSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX));

    current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
    ExpectEq(current, "0");

    current = GetCgKnob("memory", name, "memory.recharge_on_pgfault");
    ExpectEq(current, "0");

    current = GetCgKnob("cpu", name, "cpu.smart");
    ExpectEq(current, "0");
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check custom limits" << std::endl;
    string exp_limit = "52428800";
    string exp_guar = "16384";
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", "1"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectEq(alias, "1");
    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", "1k"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectEq(alias, "1024");
    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", "12m"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectEq(alias, "12582912");
    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", "123g"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectEq(alias, "132070244352");

    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", exp_limit));
    ExpectApiSuccess(api.SetProperty(name, "memory.low_limit_in_bytes", exp_guar));
    ExpectApiSuccess(api.SetProperty(name, "memory.recharge_on_pgfault", "1"));
    ExpectApiSuccess(api.SetProperty(name, "cpu.smart", "1"));

    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectApiSuccess(api.GetProperty(name, "memory_limit", real));
    ExpectEq(alias, real);
    ExpectApiSuccess(api.GetProperty(name, "memory.low_limit_in_bytes", alias));
    ExpectApiSuccess(api.GetProperty(name, "memory_guarantee", real));
    ExpectEq(alias, real);
    ExpectApiSuccess(api.GetProperty(name, "memory.recharge_on_pgfault", alias));
    ExpectApiSuccess(api.GetProperty(name, "recharge_on_pgfault", real));
    ExpectEq(alias, "1");
    ExpectEq(real, "true");
    ExpectApiSuccess(api.GetProperty(name, "cpu.smart", alias));
    ExpectApiSuccess(api.GetProperty(name, "cpu_policy", real));
    ExpectEq(alias, "1");
    ExpectEq(real, "rt");

    ExpectApiSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);
    current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
    ExpectEq(current, exp_guar);

    current = GetCgKnob("memory", name, "memory.recharge_on_pgfault");
    ExpectEq(current, "1");

    current = GetCgKnob("cpu", name, "cpu.smart");
    ExpectEq(current, "1");
    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.Destroy(name));
}

static void TestDynamic(TPortoAPI &api) {
    string name = "a";
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    string current;
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX));

    string exp_limit = "268435456";
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);

    ExpectApiSuccess(api.Pause(name));

    exp_limit = "536870912";
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);

    ExpectApiSuccess(api.Resume(name));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestLimitsHierarchy(TPortoAPI &api) {
    if (!HaveLowLimit())
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

    ExpectApiSuccess(api.Create(box));
    ExpectApiSuccess(api.Create(prod));
    ExpectApiSuccess(api.Create(slot1));
    ExpectApiSuccess(api.Create(slot2));
    ExpectApiSuccess(api.Create(system));
    ExpectApiSuccess(api.Create(monit));

    size_t total = GetTotalMemory();

    Say() << "Single container can't go over reserve" << std::endl;
    ExpectApiFailure(api.SetProperty(system, "memory_guarantee", std::to_string(total)), EError::ResourceNotAvailable);
    ExpectApiSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(total - config().daemon().memory_guarantee_reserve())));

    Say() << "Distributed guarantee can't go over reserve" << std::endl;
    size_t chunk = (total - config().daemon().memory_guarantee_reserve()) / 4;

    ExpectApiSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(chunk)));
    ExpectApiSuccess(api.SetProperty(monit, "memory_guarantee", std::to_string(chunk)));
    ExpectApiSuccess(api.SetProperty(slot1, "memory_guarantee", std::to_string(chunk)));
    ExpectApiFailure(api.SetProperty(slot2, "memory_guarantee", std::to_string(chunk + 1)), EError::ResourceNotAvailable);
    ExpectApiSuccess(api.SetProperty(slot2, "memory_guarantee", std::to_string(chunk)));

    ExpectApiSuccess(api.SetProperty(monit, "memory_guarantee", std::to_string(0)));
    ExpectApiSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(0)));

    auto CheckPropertyHierarhcy = [&](TPortoAPI &api, const std::string &property) {
        Say() << "Parent can't have less guarantee than sum of children" << std::endl;
        ExpectApiSuccess(api.SetProperty(slot1, property, std::to_string(chunk)));
        ExpectApiSuccess(api.SetProperty(slot2, property, std::to_string(chunk)));
        ExpectApiFailure(api.SetProperty(prod, property, std::to_string(chunk)), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(box, property, std::to_string(chunk)), EError::InvalidValue);

        Say() << "Child can't go over parent guarantee" << std::endl;
        ExpectApiSuccess(api.SetProperty(prod, property, std::to_string(2 * chunk)));
        ExpectApiFailure(api.SetProperty(slot1, property, std::to_string(2 * chunk)), EError::InvalidValue);

        Say() << "Can lower guarantee if possible" << std::endl;
        ExpectApiFailure(api.SetProperty(prod, property, std::to_string(chunk)), EError::InvalidValue);
        ExpectApiSuccess(api.SetProperty(slot2, property, std::to_string(0)));
        ExpectApiSuccess(api.SetProperty(prod, property, std::to_string(chunk)));
    };

    CheckPropertyHierarhcy(api, "memory_guarantee");
    CheckPropertyHierarhcy(api, "memory_limit");

    ExpectApiSuccess(api.Destroy(monit));
    ExpectApiSuccess(api.Destroy(system));
    ExpectApiSuccess(api.Destroy(slot2));
    ExpectApiSuccess(api.Destroy(slot1));
    ExpectApiSuccess(api.Destroy(prod));
    ExpectApiSuccess(api.Destroy(box));

    Say() << "Test child-parent isolation" << std::endl;

    string parent = "parent";
    string child = "parent/child";

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.SetProperty(parent, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(parent));

    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(child, "isolate", "false"));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));

    string exp_limit = "268435456";
    ExpectApiSuccess(api.SetProperty(child, "memory_limit", exp_limit));
    ExpectApiFailure(api.SetProperty(child, "hostname", "qwerty"), EError::NotSupported);
    ExpectApiSuccess(api.SetProperty(child, "cpu_limit", "10"));
    ExpectApiSuccess(api.SetProperty(child, "cpu_guarantee", "10"));
    ExpectApiSuccess(api.SetProperty(child, "respawn", "true"));

    ExpectApiSuccess(api.Start(child));

    string v;
    ExpectApiSuccess(api.GetData(parent, "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetData(child, "state", v));
    ExpectEq(v, "running");

    string current = GetCgKnob("memory", child, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);
    current = GetCgKnob("memory", parent, "memory.limit_in_bytes");
    ExpectNeq(current, exp_limit);

    string parentProperty, childProperty;
    ExpectApiSuccess(api.GetProperty(parent, "stdout_path", parentProperty));
    ExpectApiSuccess(api.GetProperty(child, "stdout_path", childProperty));
    ExpectNeq(parentProperty, childProperty);
    ExpectApiSuccess(api.GetProperty(parent, "stderr_path", parentProperty));
    ExpectApiSuccess(api.GetProperty(child, "stderr_path", childProperty));
    ExpectNeq(parentProperty, childProperty);

    string parentPid, childPid;

    ExpectApiSuccess(api.GetData(parent, "root_pid", parentPid));
    ExpectApiSuccess(api.GetData(child, "root_pid", childPid));

    AsRoot(api);

    auto parentCgmap = GetCgroups(parentPid);
    auto childCgmap = GetCgroups(childPid);

    ExpectNeq(parentCgmap["freezer"], childCgmap["freezer"]);
    ExpectNeq(parentCgmap["memory"], childCgmap["memory"]);
    if (NetworkEnabled())
        ExpectNeq(parentCgmap["net_cls"], childCgmap["net_cls"]);
    ExpectNeq(parentCgmap["cpu"], childCgmap["cpu"]);
    ExpectNeq(parentCgmap["cpuacct"], childCgmap["cpuacct"]);

    ExpectEq(GetCwd(parentPid), GetCwd(childPid));

    for (auto &ns : namespaces)
        ExpectEq(GetNamespace(parentPid, ns), GetNamespace(childPid, ns));

    ExpectApiSuccess(api.Destroy(child));
    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Test resume/pause propagation" << std::endl;
    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.SetProperty(parent, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(parent));

    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));

    std::string parentState, childState;
    ExpectApiSuccess(api.Pause(parent));
    ExpectApiSuccess(api.GetData(parent, "state", parentState));
    ExpectApiSuccess(api.GetData(child, "state", childState));
    ExpectEq(parentState, "paused");
    ExpectEq(childState, "paused");

    ExpectApiSuccess(api.Resume(parent));
    ExpectApiSuccess(api.GetData(parent, "state", parentState));
    ExpectApiSuccess(api.GetData(child, "state", childState));
    ExpectEq(parentState, "running");
    ExpectEq(childState, "running");

    ExpectApiSuccess(api.Pause(parent));
    ExpectApiFailure(api.Resume(child), EError::InvalidState);

    ExpectApiFailure(api.Destroy(child), EError::InvalidState);
    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Test mixed tree resume/pause" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.Create("a/b/c"));
    ExpectApiSuccess(api.Create("a/b/d"));

    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b/c", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b/d", "command", "true"));

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectState(api, "a", "running");
    ExpectState(api, "a/b", "meta");
    ExpectState(api, "a/b/c", "running");
    ExpectState(api, "a/b/d", "stopped");

    ExpectApiSuccess(api.Pause("a"));
    ExpectState(api, "a", "paused");
    ExpectState(api, "a/b", "paused");
    ExpectState(api, "a/b/c", "paused");
    ExpectState(api, "a/b/d", "stopped");

    ExpectApiFailure(api.Resume("a/b/c"), EError::InvalidState);
    ExpectApiFailure(api.Destroy("a/b/c"), EError::InvalidState);
    ExpectApiFailure(api.Start("a/b/d"), EError::InvalidState);

    ExpectApiSuccess(api.Resume("a"));
    ExpectState(api, "a", "running");
    ExpectState(api, "a/b", "meta");
    ExpectState(api, "a/b/c", "running");
    ExpectState(api, "a/b/d", "stopped");

    ExpectApiSuccess(api.Pause("a"));
    ExpectApiSuccess(api.Destroy("a"));

    Say() << "Test property propagation" << std::endl;
    std::string val;

    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.Create("a/b/c"));
    ExpectApiSuccess(api.SetProperty("a", "root", "/tmp"));

    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));
    ExpectApiFailure(api.SetProperty("a/b", "root", "/tmp"), EError::NotSupported);
    ExpectApiSuccess(api.SetProperty("a/b/c", "isolate", "false"));

    ExpectApiSuccess(api.GetProperty("a/b", "root", val));
    ExpectEq(val, "/tmp");
    ExpectApiSuccess(api.GetProperty("a/b/c", "root", val));
    ExpectEq(val, "/tmp");

    ExpectApiSuccess(api.SetProperty("a", "memory_limit", "12345"));
    ExpectApiSuccess(api.GetProperty("a/b", "memory_limit", val));
    ExpectNeq(val, "12345");
    ExpectApiSuccess(api.GetProperty("a/b/c", "memory_limit", val));
    ExpectNeq(val, "12345");

    ExpectApiSuccess(api.Destroy("a"));
}

static void TestPermissions(TPortoAPI &api) {
    struct stat st;
    string path;

    string name = "a";
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    path = "/sys/fs/cgroup/memory/porto";
    ExpectEq(lstat(path.c_str(), &st), 0);
    ExpectEq(st.st_mode, (0755 | S_IFDIR));

    path = "/sys/fs/cgroup/memory/porto/" + name;
    ExpectEq(lstat(path.c_str(), &st), 0);
    ExpectEq(st.st_mode, (0755 | S_IFDIR));

    path = "/sys/fs/cgroup/memory/porto/" + name + "/tasks";
    ExpectEq(lstat(path.c_str(), &st), 0);
    ExpectEq(st.st_mode, (0644 | S_IFREG));

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));

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
           ExpectApiSuccess(api.Create(name));

    AsUser(api, binUser, binGroup);
           ExpectApiFailure(api.Start(name), EError::Permission);
           ExpectApiFailure(api.Destroy(name), EError::Permission);
           ExpectApiFailure(api.SetProperty(name, "command", "sleep 1000"), EError::Permission);
           ExpectApiSuccess(api.GetProperty(name, "command", s));

    AsUser(api, daemonUser, daemonGroup);
        ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectApiFailure(api.SetProperty(name, "user", "mail"), EError::Permission);
        ExpectApiFailure(api.SetProperty(name, "group", "mail"), EError::Permission);
        ExpectApiSuccess(api.GetProperty(name, "command", s));
        ExpectApiSuccess(api.Start(name));
        ExpectApiSuccess(api.GetData(name, "root_pid", s));

    AsUser(api, binUser, binGroup);
        ExpectApiSuccess(api.GetData(name, "root_pid", s));
        ExpectApiFailure(api.Stop(name), EError::Permission);
        ExpectApiFailure(api.Pause(name), EError::Permission);

    AsUser(api, daemonUser, daemonGroup);
        ExpectApiSuccess(api.Pause(name));

    AsUser(api, binUser, binGroup);
        ExpectApiFailure(api.Destroy(name), EError::Permission);
        ExpectApiFailure(api.Resume(name), EError::Permission);

    AsRoot(api);
    ExpectApiSuccess(api.Destroy(name));
    AsNobody(api);

    Say() << "Make sure we can't create child for parent with different uid/gid " << std::endl;
    AsUser(api, binUser, binGroup);
           ExpectApiSuccess(api.Create("a"));

    AsUser(api, daemonUser, daemonGroup);
           ExpectApiFailure(api.Create("a/b"), EError::Permission);

    AsUser(api, binUser, binGroup);
           ExpectApiSuccess(api.Destroy("a"));
}

static void WaitRespawn(TPortoAPI &api, const std::string &name, int expected, int maxTries = 10) {
    std::string respawnCount;
    int successRespawns = 0;
    for(int i = 0; i < maxTries; i++) {
        sleep(config().container().respawn_delay_ms() / 1000);
        ExpectApiSuccess(api.GetData(name, "respawn_count", respawnCount));
        if (respawnCount == std::to_string(expected))
            successRespawns++;
        if (successRespawns == 2)
            break;
        Say() << "Respawned " << i << " times" << std::endl;
    }
    ExpectEq(std::to_string(expected), respawnCount);
}

static void TestRespawnProperty(TPortoAPI &api) {
    string pid, respawnPid;
    string ret;

    string name = "a";
    ExpectApiSuccess(api.Create(name));
    ExpectApiFailure(api.SetProperty(name, "max_respawns", "true"), EError::InvalidValue);

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1"));

    ExpectApiSuccess(api.SetProperty(name, "respawn", "false"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "respawn_count", ret));
    ExpectEq(ret, string("0"));
    WaitContainer(api, name);
    sleep(config().container().respawn_delay_ms() / 1000);
    ExpectApiSuccess(api.GetData(name, "respawn_count", ret));
    ExpectEq(ret, string("0"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    WaitContainer(api, name);
    WaitState(api, name, "running");
    ExpectApiSuccess(api.GetData(name, "root_pid", respawnPid));
    ExpectNeq(pid, respawnPid);
    ExpectApiSuccess(api.GetData(name, "respawn_count", ret));
    Expect(ret != "0" && ret != "");
    ExpectApiSuccess(api.Stop(name));

    int expected = 3;
    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.SetProperty(name, "max_respawns", std::to_string(expected)));
    ExpectApiSuccess(api.SetProperty(name, "command", "echo test"));
    ExpectApiSuccess(api.Start(name));

    WaitRespawn(api, name, expected);

    ExpectApiSuccess(api.Destroy(name));
}

static void ReadPropsAndData(TPortoAPI &api, const std::string &name) {
    static const std::set<std::string> skipNet = {
        "net",
        "net_tos",
        "ip",
        "default_gw",
        "net_guarantee",
        "net_limit",
        "net_priority",

        "net_bytes",
        "net_packets",
        "net_drops",
        "net_overlimits",
    };

    std::vector<TProperty> plist;
    std::vector<TData> dlist;

    ExpectApiSuccess(api.Plist(plist));
    ExpectApiSuccess(api.Dlist(dlist));

    if (!NetworkEnabled()) {
        plist.erase(std::remove_if(plist.begin(),
                                   plist.end(),
                                   [&](TProperty &p){
                                       return skipNet.find(p.Name) != skipNet.end();
                                   }),
                    plist.end());

        dlist.erase(std::remove_if(dlist.begin(),
                                   dlist.end(),
                                   [&](TData &d){
                                       return skipNet.find(d.Name) != skipNet.end();
                                   }),
                    dlist.end());
    }

    std::string v;

    for (auto p : plist)
        (void)api.GetProperty(name, p.Name, v);

    for (auto d : dlist)
        (void)api.GetData(name, d.Name, v);
}

static void TestLeaks(TPortoAPI &api) {
    string slavePid, masterPid;
    string name;
    int slack = 4096 * 2;

    TFile slaveFile(config().slave_pid().path());
    ExpectSuccess(slaveFile.AsString(slavePid));
    TFile masterFile(config().master_pid().path());
    ExpectSuccess(masterFile.AsString(masterPid));

    int prevSlave = GetVmRss(slavePid);
    int prevMaster = GetVmRss(masterPid);

    int createDestroyNr = 50000;

    Say() << "Create and destroy single container " << createDestroyNr << " times" << std::endl;
    name = "a";
    for (int i = 0; i < createDestroyNr; i++) {
        ExpectApiSuccess(api.Create(name));
        api.Cleanup();
        ExpectApiSuccess(api.Destroy(name));
        api.Cleanup();
    }

    int nowSlave = GetVmRss(slavePid);
    int nowMaster = GetVmRss(masterPid);

    Say() << "Expected slave " << nowSlave << " < " << prevSlave + slack << std::endl;
    Expect(nowSlave <= prevSlave + slack);

    Say() << "Expected master " << nowMaster << " < " << prevMaster + slack << std::endl;
    Expect(nowMaster <= prevMaster + slack);

    Say() << "Create and destroy " << LeakConainersNr << " containers" << std::endl;
    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "true"));
        ExpectApiSuccess(api.Start(name));

        ReadPropsAndData(api, name);
    }

    name = "a0";
    for (int i = 0; i < LeakConainersNr; i++)
        ReadPropsAndData(api, name);

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ExpectApiSuccess(api.Destroy(name));
    }

    prevSlave = GetVmRss(slavePid);
    prevMaster = GetVmRss(masterPid);

    Say() << "Create and destroy " << LeakConainersNr << " containers, current RSS " << prevMaster << "/" << prevSlave << std::endl;

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "b" + std::to_string(i);
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "true"));
        ExpectApiSuccess(api.Start(name));
        ReadPropsAndData(api, name);
        api.Cleanup();
    }

    name = "b0";
    for (int i = 0; i < LeakConainersNr; i++)
        ReadPropsAndData(api, name);

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "b" + std::to_string(i);
        ExpectApiSuccess(api.Destroy(name));
        api.Cleanup();
    }

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    Say() << "Expected slave " << nowSlave << " < " << prevSlave + slack << std::endl;
    Expect(nowSlave <= prevSlave + slack);

    Say() << "Expected master " << nowMaster << " < " << prevMaster + slack << std::endl;
    Expect(nowMaster <= prevMaster + slack);
}

static void TestPerf(TPortoAPI &api) {
    std::string name, v;
    size_t begin, ms;
    const int nr = 1000;
    const int createMs = 60;
    const int getStateMs = 1;
    const int destroyMs = 120;

    begin = GetCurrentTimeMs();
    for (int i = 0; i < nr; i++) {
        name = "perf" + std::to_string(i);
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectApiSuccess(api.Start(name));
    }
    ms = GetCurrentTimeMs() - begin;
    Say() << "Create " << nr << " containers took " << ms / 1000.0 << "s" << std::endl;
    Expect(ms < createMs * nr);

    begin = GetCurrentTimeMs();
    for (int i = 0; i < nr; i++) {
        name = "perf" + std::to_string(i);
        ExpectApiSuccess(api.GetData(name, "state", v));
    }
    ms = GetCurrentTimeMs() - begin;
    Say() << "Get state " << nr << " containers took " << ms / 1000.0 << "s" << std::endl;
    Expect(ms < getStateMs * nr);

    std::vector<std::string> containers;
    std::vector<std::string> variables = { "state" };
    std::map<std::string, std::map<std::string, TPortoGetResponse>> result;

    for (int i = 0; i < nr; i++)
        containers.push_back("perf" + std::to_string(i));

    begin = GetCurrentTimeMs();
    ExpectApiSuccess(api.Get(containers, variables, result));
    ms = GetCurrentTimeMs() - begin;

    Say() << "Combined get state " << nr << " took " << ms / 1000.0 << "s" << std::endl;
    Expect(ms < getStateMs * nr);
    ExpectEq(result.size(), nr);

    begin = GetCurrentTimeMs();
    for (int i = 0; i < nr; i++) {
        name = "perf" + std::to_string(i);
        ExpectApiSuccess(api.Destroy(name));
    }
    ms = GetCurrentTimeMs() - begin;
    Say() << "Destroy " << nr << " containers took " << ms / 1000.0 << "s" << std::endl;
    Expect(ms < destroyMs * nr);
}

static void CleanupVolume(TPortoAPI &api, const std::string &path) {
    AsRoot(api);
    TFolder dir(path);
    if (dir.Exists()) {
        TError error = dir.Remove(true);
        if (error)
            throw error.GetMsg();
    }
    AsNobody(api);
}

static void TestVolumeHolder(TPortoAPI &api) {
    std::vector<TVolumeDescription> volumes;

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 0);

    std::string a = "/tmp/volume_a";
    std::map<std::string, std::string> prop_default = {};
    std::map<std::string, std::string> prop_invalid = {{"foo", "bar"}};

    CleanupVolume(api, a);

    TPath aPath(a);
    ExpectEq(aPath.Exists(), false);

    ExpectSuccess(aPath.Mkdir(0775));

    Say() << "Create volume A" << std::endl;
    ExpectApiSuccess(api.CreateVolume(a, prop_default));

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 1);
    ExpectEq(volumes[0].Path, a);
    ExpectEq(volumes[0].Containers.size(), 1);

    ExpectEq(volumes[0].Properties.count("ready"), 1);
    ExpectEq(volumes[0].Properties.count("backend"), 1);
    ExpectEq(volumes[0].Properties.count("user"), 1);
    ExpectEq(volumes[0].Properties.count("group"), 1);
    ExpectEq(volumes[0].Properties.count("permissions"), 1);
    ExpectEq(volumes[0].Properties.count("creator"), 1);

    ExpectEq(volumes[0].Properties.count("space_used"), 1);
    ExpectEq(volumes[0].Properties.count("space_available"), 1);
    ExpectEq(volumes[0].Properties.count("inode_used"), 1);
    ExpectEq(volumes[0].Properties.count("inode_available"), 1);

    ExpectEq(aPath.Exists(), true);

    Say() << "Try to create existing volume A" << std::endl;
    ExpectApiFailure(api.CreateVolume(a, prop_default), EError::VolumeAlreadyExists);

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 1);

    /* Anon volume */
    std::string b = "";

    Say() << "Create volume B" << std::endl;
    ExpectApiSuccess(api.CreateVolume(b, prop_default));

    TPath bPath(b);
    ExpectEq(bPath.Exists(), true);

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 2);

    ExpectEq(volumes[0].Containers.size(), 1);
    ExpectEq(volumes[1].Containers.size(), 1);

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(b, "", volumes));
    ExpectEq(volumes.size(), 1);
    ExpectEq(volumes[0].Path, b);

    ExpectEq(aPath.Exists(), true);
    ExpectEq(bPath.Exists(), true);

    Say() << "Remove volume A" << std::endl;
    ExpectApiSuccess(api.UnlinkVolume(a, ""));
    ExpectApiFailure(api.UnlinkVolume(a, ""), EError::VolumeNotFound);

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 1);
    ExpectEq(volumes[0].Path, b);
    ExpectEq(volumes[0].Containers.size(), 1);

    ExpectEq(aPath.Exists(), true);
    ExpectEq(bPath.Exists(), true);

    Say() << "Remove volume B" << std::endl;
    ExpectApiSuccess(api.UnlinkVolume(b, ""));
    ExpectApiFailure(api.UnlinkVolume(b, ""), EError::VolumeNotFound);

    ExpectEq(aPath.Exists(), true);
    ExpectEq(bPath.Exists(), false);

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 0);

    Say() << "Try to create volume with invalid path" << std::endl;
    b = "b";
    ExpectApiFailure(api.CreateVolume(b, prop_default), EError::InvalidValue);
    ExpectApiFailure(api.CreateVolume(a, prop_invalid), EError::InvalidValue);
}

static void TestVolumeImpl(TPortoAPI &api) {
    std::vector<TVolumeDescription> volumes;
    std::map<std::string, std::string> prop_loop = {{"backend", "loop"}, {"space_limit", "100m"}};
    std::map<std::string, std::string> prop_limited = {{"space_limit", "100m"}, {"inode_limit", "1000"}};
    std::map<std::string, std::string> prop_unlimit = {};
    //uint64_t usage, limit, avail, guarantee;

    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 0);

    std::string a, b;

    CleanupVolume(api, a);
    CleanupVolume(api, b);

    ExpectApiSuccess(api.CreateVolume(a, prop_loop));
    ExpectApiSuccess(api.CreateVolume(b, prop_unlimit));

    Say() << "Make mountpoint is created" << std::endl;

    vector<string> v;
    ExpectSuccess(Popen("cat /proc/self/mountinfo", v));
    auto m = ParseMountinfo(CommaSeparatedList(v, ""));
    Expect(m.find(a) != m.end());
    Expect(m.find(b) != m.end());

    if (false) {

        // TODO:
        // - test quota when ready
        // - make sure overlayfs upper/lower/work dirs are correct
    } else {
        Say() << "Make sure loop device has created" << std::endl;
        Expect(StringStartsWith(m[a].source, "/dev/loop"));
        std::string loopDev = m[a].source;
        AsRoot(api);
        std::string img = System("losetup " + loopDev + " | sed -e 's/[^(]*(\\([^)]*\\)).*/\\1/'");
        AsNobody(api);

        Say() << "Make sure loop device has correct size" << std::endl;
        TFile loopFile(img);
        off_t expected = 100 * 1024 * 1024;
        off_t mistake = 1 * 1024 * 1024;
        Expect(loopFile.GetSize() > expected - mistake && loopFile.GetSize() < expected + mistake);

        Say() << "Make sure no loop device is created without quota" << std::endl;
        Expect(!StringStartsWith(m[b].source, "/dev/loop"));
    }

    /*
    ExpectSuccess(StringToUint64(volumes[0].Properties["space_usage"], usage));
    ExpectSuccess(StringToUint64(volumes[0].Properties["space_limit"], limit));
    ExpectSuccess(StringToUint64(volumes[0].Properties["space_avail"], avail));
    ExpectSuccess(StringToUint64(volumes[0].Properties["space_guarantee"], guarantee));

    Expect(limit == 104857600);
    Expect(usage + avail <= limit);
    Expect(usage + avail >= guarantee);

    ExpectSuccess(StringToUint64(volumes[0].Properties["inode_usage"], usage));
    ExpectSuccess(StringToUint64(volumes[0].Properties["inode_limit"], limit));
    ExpectSuccess(StringToUint64(volumes[0].Properties["inode_avail"], avail));
    ExpectSuccess(StringToUint64(volumes[0].Properties["inode_guarantee"], guarantee));

    Expect(limit == 100);
    Expect(usage + avail <= limit);
    Expect(usage + avail >= guarantee);

    */

    ExpectApiSuccess(api.UnlinkVolume(a, ""));
    ExpectApiSuccess(api.UnlinkVolume(b));

    ExpectEq(TPath(a).Exists(), false);
    ExpectEq(TPath(b).Exists(), false);
}

static void TestSigPipe(TPortoAPI &api) {
    std::string before;
    ExpectApiSuccess(api.GetData("/", "porto_stat[spawned]", before));

    int fd;
    ExpectSuccess(ConnectToRpcServer(config().rpc_sock().file().path(), fd));

    rpc::TContainerRequest req;
    req.mutable_list();

    google::protobuf::io::FileOutputStream post(fd);
    WriteDelimitedTo(req, &post);
    post.Flush();

    close(fd);
    WaitPortod(api);

    std::string after;
    ExpectApiSuccess(api.GetData("/", "porto_stat[spawned]", after));
    ExpectEq(before, after);
}

static void KillMaster(TPortoAPI &api, int sig, int times = 10) {
    AsRoot(api);
    RotateDaemonLogs(api);
    AsNobody(api);

    int pid = ReadPid(config().master_pid().path());
    if (kill(pid, sig))
        throw "Can't send " + std::to_string(sig) + " to master";
    WaitProcessExit(std::to_string(pid));
    WaitPortod(api, times);

    std::string v;
    ExpectApiSuccess(api.GetData("/", "porto_stat[spawned]", v));
    ExpectEq(v, "1");

    expectedErrors = expectedRespawns = expectedWarns = 0;
}

static void KillSlave(TPortoAPI &api, int sig, int times = 10) {
    int portodPid = ReadPid(config().slave_pid().path());
    if (kill(portodPid, sig))
        throw "Can't send " + std::to_string(sig) + " to slave";
    WaitProcessExit(std::to_string(portodPid));
    WaitPortod(api, times);
    expectedRespawns++;

    std::string v;
    ExpectApiSuccess(api.GetData("/", "porto_stat[spawned]", v));
    ExpectEq(v, std::to_string(expectedRespawns + 1));
}

static bool RespawnTicks(TPortoAPI &api, const std::string &name, int maxTries = 3) {
    std::string respawnCount, v;
    ExpectApiSuccess(api.GetData(name, "respawn_count", respawnCount));
    for(int i = 0; i < maxTries; i++) {
        sleep(config().container().respawn_delay_ms() / 1000);
        ExpectApiSuccess(api.GetData(name, "respawn_count", v));

        if (v != respawnCount)
            return true;
    }
    return false;
}

static void TestWait(TPortoAPI &api) {
    std::string c = "aaa";
    std::string d = "aaa/bbb";
    std::string tmp;

    Say() << "Check wait for non-existing and invalid containers" << std::endl;
    ExpectApiFailure(api.Wait({c}, tmp), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Wait({"/"}, tmp), EError::Permission);
    ExpectApiFailure(api.Wait({}, tmp), EError::InvalidValue);

    Say() << "Check wait for stopped container" << std::endl;
    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.Wait({c}, tmp));
    ExpectEq(c, tmp);

    Say() << "Check wait for running/dead container" << std::endl;
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 1"));
    ExpectApiSuccess(api.Start(c));
    ExpectApiSuccess(api.Wait({c}, tmp));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetData(c, "state", tmp));
    ExpectEq(tmp, "dead");

    ExpectApiSuccess(api.Wait({c}, tmp));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetData(c, "state", tmp));
    ExpectEq(tmp, "dead");
    ExpectApiSuccess(api.Stop(c));
    ExpectApiSuccess(api.Destroy(c));

    Say() << "Check wait for containers in meta-state" << std::endl;
    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.Create(d));

    ExpectApiSuccess(api.SetProperty(d, "command", "sleep 1"));
    ExpectApiSuccess(api.Start(d));
    ExpectApiSuccess(api.GetData(c, "state", tmp));
    ExpectEq(tmp, "meta");
    ExpectApiSuccess(api.Wait({c}, tmp));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.Stop(d));
    ExpectApiSuccess(api.Destroy(d));
    ExpectApiSuccess(api.Stop(c));
    ExpectApiSuccess(api.Destroy(c));

    Say() << "Check wait for large number of containers" << std::endl;
    std::vector<std::string> containers;
    for (int i = 0; i < 100; i++)
        containers.push_back(c + std::to_string(i));
    for (auto &name : containers) {
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectApiSuccess(api.Start(name));
        ExpectApiSuccess(api.GetData(name, "state", tmp));
        ExpectEq(tmp, "running");
    }

    ExpectApiSuccess(api.Kill(containers[50], 9));
    ExpectApiSuccess(api.Wait(containers, tmp));
    ExpectEq(tmp, containers[50]);
    ExpectApiSuccess(api.GetData(containers[50], "state", tmp));
    ExpectEq(tmp, "dead");

    for (auto &name : containers)
        ExpectApiSuccess(api.Destroy(name));

    Say() << "Check wait timeout" << std::endl;
    size_t begin, end;

    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(c));

    begin = GetCurrentTimeMs();
    ExpectApiSuccess(api.Wait({c}, tmp, 0));
    end = GetCurrentTimeMs();
    ExpectEq(tmp, "");
    Expect(end - begin < 100);

    begin = GetCurrentTimeMs();
    ExpectApiSuccess(api.Wait({c}, tmp, 2000));
    end = GetCurrentTimeMs();
    ExpectEq(tmp, "");
    Expect(end - begin >= 2000);

    ExpectApiSuccess(api.Destroy(c));
}

static void TestWaitRecovery(TPortoAPI &api) {
    std::string c = "aaa";
    std::string d = "aaa/bbb";
    std::string tmp;

    Say() << "Check wait for restored container" << std::endl;

    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 3"));
    ExpectApiSuccess(api.Start(c));

    KillSlave(api, SIGKILL);

    ExpectApiSuccess(api.Wait({c}, tmp));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetData(c, "state", tmp));
    ExpectEq(tmp, "dead");
    ExpectApiSuccess(api.Stop(c));

    Say() << "Check wait for lost and restored container" << std::endl;
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 3"));
    ExpectApiSuccess(api.Start(c));

    KillMaster(api, SIGKILL);

    ExpectApiSuccess(api.Wait({c}, tmp));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetData(c, "state", tmp));
    ExpectEq(tmp, "dead");
    ExpectApiSuccess(api.Stop(c));
    ExpectApiSuccess(api.Destroy(c));
}

static void TestRecovery(TPortoAPI &api) {
    string pid, v;
    string name = "a:b";
    std::vector<std::string> containers;

    map<string,string> props = {
        { "command", "sleep 1000" },
        { "user", "bin" },
        { "group", "daemon" },
        { "env", "a=a; b=b" },
    };

    Say() << "Make sure we can restore stopped child when parent is dead" << std::endl;

    ExpectApiSuccess(api.Create("parent"));
    ExpectApiSuccess(api.Create("parent/child"));
    ExpectApiSuccess(api.SetProperty("parent", "command", "sleep 1"));
    ExpectApiSuccess(api.SetProperty("parent/child", "command", "sleep 2"));
    ExpectApiSuccess(api.Start("parent"));
    ExpectApiSuccess(api.Start("parent/child"));
    ExpectApiSuccess(api.Stop("parent/child"));
    WaitContainer(api, "parent");

    KillMaster(api, SIGKILL);

    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 3);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("parent"));
    ExpectEq(containers[2], string("parent/child"));

    ExpectApiSuccess(api.Destroy("parent"));

    Say() << "Make sure we can figure out that containers are dead even if master dies" << std::endl;

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 3"));
    ExpectApiSuccess(api.Start(name));

    KillMaster(api, SIGKILL);
    WaitContainer(api, name);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure we don't kill containers when doing recovery" << std::endl;

    AsRoot(api);
    ExpectApiSuccess(api.Create(name));

    for (auto &pair : props)
        ExpectApiSuccess(api.SetProperty(name, pair.first, pair.second));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.SetProperty(name, "private", "ISS-AGENT"));

    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);
    ExpectEq(TaskZombie(pid), false);

    KillSlave(api, SIGKILL);

    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetData(name, "root_pid", v));
    ExpectEq(v, pid);

    ExpectEq(TaskRunning(pid), true);
    ExpectEq(TaskZombie(pid), false);

    for (auto &pair : props) {
        string v;
        ExpectApiSuccess(api.GetProperty(name, pair.first, v));
        ExpectEq(v, pair.second);
    }

    ExpectApiSuccess(api.Destroy(name));
    AsNobody(api);

    Say() << "Make sure meta gets correct state upon recovery" << std::endl;
    string parent = "a";
    string child = "a/b";

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(parent, "isolate", "true"));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));

    AsRoot(api);
    KillSlave(api, SIGKILL);
    AsNobody(api);

    ExpectApiSuccess(api.GetData(parent, "state", v));
    ExpectEq(v, "meta");

    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Make sure hierarchical recovery works" << std::endl;

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(parent, "isolate", "false"));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));

    AsRoot(api);
    KillSlave(api, SIGKILL);
    AsNobody(api);

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 3);
    ExpectEq(containers[0], string("/"));
    ExpectEq(containers[1], string("a"));
    ExpectEq(containers[2], string("a/b"));
    ExpectApiSuccess(api.GetData(parent, "state", v));
    ExpectEq(v, "meta");

    ExpectApiSuccess(api.SetProperty(parent, "recharge_on_pgfault", "true"));
    ExpectApiFailure(api.SetProperty(parent, "env", "a=b"), EError::InvalidState);

    ExpectApiSuccess(api.GetData(child, "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.Destroy(child));
    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Make sure task is moved to correct cgroup on recovery" << std::endl;
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    ExpectApiSuccess(api.GetData(name, "root_pid", pid));

    AsRoot(api);
    TFile f("/sys/fs/cgroup/memory/porto/cgroup.procs");
    ExpectSuccess(f.AppendString(pid));
    auto cgmap = GetCgroups(pid);
    ExpectEq(cgmap["memory"], "/porto");
    KillSlave(api, SIGKILL);
    AsNobody(api);
    expectedWarns++; // Task belongs to invalid subsystem

    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    ExpectCorrectCgroups(pid, name);
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure some data is persistent" << std::endl;
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", oomCommand));
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", oomMemoryLimit));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetData(name, "exit_status", v));
    ExpectEq(v, string("9"));
    ExpectApiSuccess(api.GetData(name, "oom_killed", v));
    ExpectEq(v, string("true"));
    KillSlave(api, SIGKILL);
    ExpectApiSuccess(api.GetData(name, "exit_status", v));
    ExpectEq(v, string("9"));
    ExpectApiSuccess(api.GetData(name, "oom_killed", v));
    ExpectEq(v, string("true"));
    ExpectApiSuccess(api.Stop(name));

    int expected = 1;
    ExpectApiSuccess(api.SetProperty(name, "command", "false"));
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "0"));
    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.SetProperty(name, "max_respawns", std::to_string(expected)));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    KillSlave(api, SIGKILL);
    WaitRespawn(api, name, expected);
    ExpectApiSuccess(api.GetData(name, "respawn_count", v));
    ExpectEq(v, std::to_string(expected));

    Say() << "Make sure stopped state is persistent" << std::endl;
    ExpectApiSuccess(api.Destroy(name));
    ExpectApiSuccess(api.Create(name));
    ShouldHaveValidProperties(api, name);
    ShouldHaveValidData(api, name);
    KillSlave(api, SIGKILL);
    ExpectApiSuccess(api.GetData(name, "state", v));
    ExpectEq(v, "stopped");
    ShouldHaveValidProperties(api, name);
    ShouldHaveValidData(api, name);

    Say() << "Make sure paused state is persistent" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ShouldHaveValidRunningData(api, name);
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    v = GetState(pid);
    Expect(v == "S" || v == "R");
    ExpectApiSuccess(api.Pause(name));
    v = GetState(pid);
    //ExpectEq(v, "D");
    KillSlave(api, SIGKILL);
    ExpectApiSuccess(api.GetData(name, "root_pid", pid));
    v = GetState(pid);
    //ExpectEq(v, "D");
    ExpectApiSuccess(api.Resume(name));
    ShouldHaveValidRunningData(api, name);
    v = GetState(pid);
    Expect(v == "S" || v == "R");
    ExpectApiSuccess(api.GetData(name, "time", v));
    ExpectNeq(v, "0");
    ExpectApiSuccess(api.Destroy(name));

    if (NetworkEnabled()) {
        Say() << "Make sure network counters are persistent" << std::endl;
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'wget yandex.ru && sync'"));
        ExpectApiSuccess(api.Start(name));
        WaitContainer(api, name);

        ExpectNonZeroLink(api, name, "net_bytes");
        KillSlave(api, SIGKILL);
        ExpectNonZeroLink(api, name, "net_bytes");

        ExpectApiSuccess(api.Destroy(name));
    }

    Say() << "Make sure respawn_count ticks after recovery " << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "true"));
    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.Start(name));
    ExpectEq(RespawnTicks(api, name), true);
    KillSlave(api, SIGKILL);
    ExpectEq(RespawnTicks(api, name), true);
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure we can recover huge number of containers " << std::endl;
    const size_t nr = config().container().max_total() - 2;

    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectApiSuccess(api.Start(name));
    }

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), nr + 1);

    ExpectApiFailure(api.Create("max_plus_one"), EError::ResourceNotAvailable);

    KillSlave(api, SIGKILL, 5 * 60);

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), nr + 1);

    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectApiSuccess(api.Kill(name, SIGKILL));
    }
    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectApiSuccess(api.Destroy(name));
    }
}

static void TestVolumeFiles(TPortoAPI &api, const std::string &path) {
    vector<string> v;

    ExpectSuccess(Popen("cat /proc/self/mountinfo", v));
    auto m = ParseMountinfo(CommaSeparatedList(v, ""));
    Expect(m.find(path) != m.end());
}

static void TestVolumeRecovery(TPortoAPI &api) {
    Say() << "Make sure porto removes leftover volumes" << std::endl;
    std::string a = "/tmp/volume_c", b = "";
    std::map<std::string, std::string> prop_limited = {{"space_limit", "100m"}, {"inode_limit", "1000"}};
    std::map<std::string, std::string> prop_unlimit = {};

    CleanupVolume(api, a);
    ExpectSuccess(TPath(a).Mkdir(0775));

    std::vector<TVolumeDescription> volumes;
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 0);

    ExpectApiSuccess(api.CreateVolume(a, prop_limited));
    ExpectApiSuccess(api.CreateVolume(b, prop_unlimit));

    TFolder volume(config().volumes().volume_dir() + "/leftover_volume");
    AsRoot(api);
    volume.Remove(true);
    ExpectSuccess(volume.Create());
    AsNobody(api);

    ExpectEq(volume.Exists(), true);

    KillSlave(api, SIGKILL);

    ExpectEq(volume.Exists(), false);

    Say() << "Make sure porto preserves mounted loop/overlayfs" << std::endl;
    volumes.clear();
    ExpectApiSuccess(api.ListVolumes(volumes));
    ExpectEq(volumes.size(), 2);

    TestVolumeFiles(api, b);

    vector<string> v;
    ExpectSuccess(Popen("cat /proc/self/mountinfo", v));
    auto m = ParseMountinfo(CommaSeparatedList(v, ""));
    Expect(m.find(a) != m.end());
    Expect(m.find(b) != m.end());

    ExpectApiSuccess(api.UnlinkVolume(a));
    ExpectApiSuccess(api.UnlinkVolume(b, ""));

    v.clear();
    ExpectSuccess(Popen("cat /proc/self/mountinfo", v));
    m = ParseMountinfo(CommaSeparatedList(v, ""));
    Expect(m.find(a) == m.end());
    Expect(m.find(b) == m.end());

    ExpectSuccess(TPath(a).Rmdir());
    ExpectEq(TPath(b).Exists(), false);
}

static void TestCgroups(TPortoAPI &api) {
    AsRoot(api);

    Say() << "Make sure we don't remove non-porto cgroups" << std::endl;

    string freezerCg = "/sys/fs/cgroup/freezer/qwerty/asdfg";

    RemakeDir(api, freezerCg);

    KillSlave(api, SIGINT);

    TFolder qwerty(freezerCg);
    ExpectEq(qwerty.Exists(), true);
    ExpectEq(qwerty.Remove(), false);

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

    KillSlave(api, SIGKILL);

    TFolder freezer(freezerCg);
    ExpectEq(freezer.Exists(), false);
    TFolder memory(memoryCg);
    ExpectEq(memory.Exists(), false);
    TFolder cpu(cpuCg);
    ExpectEq(cpu.Exists(), false);
}

static void TestVersion(TPortoAPI &api) {
    string tag, revision;
    ExpectApiSuccess(api.GetVersion(tag, revision));

    ExpectEq(tag, GIT_TAG);
    ExpectEq(revision, GIT_REVISION);
}

static void SetWorkersNr(TPortoAPI &api, size_t nr) {
    AsRoot(api);

    config().mutable_daemon()->set_workers(nr);
    TFile f("/etc/portod.conf");
    ExpectSuccess(f.WriteStringNoAppend(config().ShortDebugString()));

    KillSlave(api, SIGTERM);

    AsNobody(api);
}

static void TestBadClient(TPortoAPI &api) {
    auto defaultWorkerNr = config().daemon().workers();
    SetWorkersNr(api, 1);

    std::vector<std::string> clist;
    int sec = 120;

#if 0
    Say() << "Check client that doesn't read responses" << std::endl;

    ExpectApiSuccess(api.List(clist)); // connect to porto

    alarm(sec);
    size_t nr = 1000000;
    while (nr--) {
        rpc::TContainerRequest req;
        req.mutable_propertylist();
        api.Send(req);

        if (nr && nr % 100000 == 0)
            Say() << nr << " left" << std::endl;
    }
    alarm(0);
#endif

    Say() << "Check client that does partial write" << std::endl;

    int fd;
    string buf = "xyz";
    alarm(sec);
    ExpectApiSuccess(ConnectToRpcServer(config().rpc_sock().file().path(), fd));
    ExpectEq(write(fd, buf.c_str(), buf.length()), buf.length());

    TPortoAPI api2(config().rpc_sock().file().path(), 0);
    ExpectApiSuccess(api2.List(clist));
    close(fd);
    alarm(0);

    SetWorkersNr(api, defaultWorkerNr);
}

static void SetLogRotateTimeout(TPortoAPI &api, size_t s) {
    AsRoot(api);

    config().mutable_daemon()->set_rotate_logs_timeout_s(s);
    TFile f("/etc/portod.conf");
    ExpectSuccess(f.WriteStringNoAppend(config().ShortDebugString()));

    KillSlave(api, SIGTERM);

    AsNobody(api);
}

static void TestRemoveDead(TPortoAPI &api) {
    std::string v;
    ExpectApiSuccess(api.GetData("/", "porto_stat[remove_dead]", v));
    ExpectEq(v, std::to_string(0));

    auto defaultTimeout = config().daemon().rotate_logs_timeout_s();
    SetLogRotateTimeout(api, 1);

    std::string name = "dead";
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "true"));
    ExpectApiSuccess(api.SetProperty(name, "aging_time", "1"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);

    usleep(2 * 1000 * 1000);
    std::string state;
    ExpectApiFailure(api.GetData(name, "state", state), EError::ContainerDoesNotExist);

    ExpectApiSuccess(api.GetData("/", "porto_stat[remove_dead]", v));
    ExpectEq(v, std::to_string(1));

    SetLogRotateTimeout(api, defaultTimeout);
}

static void TestLogRotate(TPortoAPI &api) {
    std::string v;
    auto defaultTimeout = config().daemon().rotate_logs_timeout_s();
    SetLogRotateTimeout(api, 2);

    std::string name = "biglog";
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.GetProperty(name, "stdout_path", v));
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'dd if=/dev/zero bs=1M count=100 && sleep 5'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);

    TPath stdoutPath(v);
    ExpectLess(stdoutPath.GetDiskUsage(), config().container().max_log_size());

    SetLogRotateTimeout(api, defaultTimeout);
}

static void TestStats(TPortoAPI &api) {
    if (!needDaemonChecks)
        return;

    AsRoot(api);

    int respawns = WordCount(config().master_log().path(), "SYS Spawned");
    int errors = WordCount(config().slave_log().path(), "ERR ");
    int warns = WordCount(config().slave_log().path(), "WRN ");

    std::string v;
    ExpectApiSuccess(api.GetData("/", "porto_stat[spawned]", v));
    ExpectEq(v, std::to_string(respawns));

    ExpectApiSuccess(api.GetData("/", "porto_stat[errors]", v));
    ExpectEq(v, std::to_string(errors));

    ExpectApiSuccess(api.GetData("/", "porto_stat[warnings]", v));
    ExpectEq(v, std::to_string(warns));

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

    ExpectEq(system("stop yandex-porto"), 0);

    Expect(FileExists(config().master_log().path()));
    Expect(FileExists(config().slave_log().path()));
    ExpectEq(FileExists(config().rpc_sock().file().path()), false);

    ExpectEq(system("start yandex-porto"), 0);
    WaitPortod(api);
}

int SelfTest(std::vector<std::string> name, int leakNr) {
    pair<string, std::function<void(TPortoAPI &)>> tests[] = {
        { "path", TestPath },
        { "idmap", TestIdmap },
        { "root", TestRoot },
        { "data", TestData },
        { "holder", TestHolder },
        { "get", TestGet },
        { "meta", TestMeta },
        { "empty", TestEmpty },
        { "state_machine", TestStateMachine },
        { "wait", TestWait },
        { "exit_status", TestExitStatus },
        { "streams", TestStreams },
        { "ns_cg_tc", TestNsCgTc },
        { "isolate_property", TestIsolateProperty },
        { "container_namespaces", TestContainerNamespaces },
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
        { "enable_porto_property", TestEnablePortoProperty },
        { "limits", TestLimits },
        { "ulimit_property", TestUlimitProperty },
        { "virt_mode_property", TestVirtModeProperty },
        { "alias", TestAlias },
        { "dynamic", TestDynamic },
        { "permissions", TestPermissions },
        { "respawn_property", TestRespawnProperty },
        { "hierarchy", TestLimitsHierarchy },
        { "leaks", TestLeaks },
        { "perf", TestPerf },
        { "vholder", TestVolumeHolder },
        { "volume_impl", TestVolumeImpl },
        { "sigpipe", TestSigPipe },
        { "stats", TestStats },
        { "daemon", TestDaemon },

        // the following tests will restart porto several times
        { "bad_client", TestBadClient },
        { "recovery", TestRecovery },
        { "wait_recovery", TestWaitRecovery },
        { "volume_recovery", TestVolumeRecovery },
        { "cgroups", TestCgroups },
        { "version", TestVersion },
        { "remove_dead", TestRemoveDead },
        { "log_rotate", TestLogRotate },
        { "stats", TestStats },
        { "package", TestPackage },
    };

    int ret = EXIT_SUCCESS;
    ExpectSuccess(SetHostName(HOSTNAME));

    if (NetworkEnabled())
        subsystems.push_back("net_cls");

    LeakConainersNr = leakNr;

    needDaemonChecks = getenv("NOCHECK") == nullptr;

    config.Load();
    TPortoAPI api(config().rpc_sock().file().path(), 0);

    try {
        if (needDaemonChecks) {
            RestartDaemon(api);

            ExpectEq(WordCount(config().master_log().path(), "Started"), 1);
            ExpectEq(WordCount(config().slave_log().path(), "Started"), 1);
        }

        TGroup portoGroup("porto");
        TError error = portoGroup.Load();
        if (error)
            throw error.GetMsg();

        gid_t portoGid = portoGroup.GetId();
        ExpectEq(setgroups(1, &portoGid), 0);

        for (auto t : tests) {
            if (name.size() &&
                std::find(name.begin(), name.end(), t.first) == name.end())
                continue;

            std::cerr << ">>> Testing " << t.first << "..." << std::endl;
            AsNobody(api);

            t.second(api);
        }

        AsRoot(api);

        if (!needDaemonChecks)
            goto exit;
    } catch (string e) {
        std::cerr << "EXCEPTION: " << e << std::endl;
        ret = EXIT_FAILURE;
        goto exit;
    }

    std::cerr << "SUCCESS: All tests successfully passed!" << std::endl;
    if (!CanTestLimits())
        std::cerr << "WARNING: Due to missing kernel support, memory_guarantee/cpu_policy has not been tested!" << std::endl;
    if (!HaveCfsBandwidth())
        std::cerr << "WARNING: CFS bandwidth is not enabled, skipping cpu_limit tests" << std::endl;
    if (!HaveCfsGroupSched())
        std::cerr << "WARNING: CFS group scheduling is not enabled, skipping cpu_guarantee tests" << std::endl;
    if (!IsCfqActive())
        std::cerr << "WARNING: CFQ is not enabled for one of your block devices, skipping io_read and io_write tests" << std::endl;
    if (!NetworkEnabled())
        std::cerr << "WARNING: Network support is not tested" << std::endl;
    if (links.size() == 1)
        std::cerr << "WARNING: Multiple network support is not tested" << std::endl;
    if (!HaveMaxRss())
        std::cerr << "WARNING: max_rss is not tested" << std::endl;
    if (!HaveIoLimit())
        std::cerr << "WARNING: io_limit is not tested" << std::endl;

exit:
    AsRoot(api);
    if (system("hostname -F /etc/hostname") != 0)
        std::cerr << "WARNING: can't restore hostname" << std::endl;
    return ret;
}
}
