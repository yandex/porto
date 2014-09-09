#include <iostream>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <cstdio>

#include "rpc.pb.h"
#include "libporto.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <pwd.h>
#include <dirent.h>
}

using namespace std;

#define Expect(ret) _ExpectFailure(ret, true, __LINE__, __func__)
#define ExpectSuccess(ret) _ExpectFailure(ret, 0, __LINE__, __func__)
#define ExpectFailure(ret, exp) _ExpectFailure(ret, exp, __LINE__, __func__)
static void _ExpectFailure(int ret, int exp, int line, const char *func) {
    if (ret != exp)
        throw string("Got " + to_string(ret) + ", but expected " + to_string(exp) + " at " + func + ":" + to_string(line));
}

static void WaitExit(TPortoAPI &api, const string &pid) {
    cerr << "Waiting for " << pid << " to exit..." << endl;

    int times = 100;
    int p = stoi(pid);

    do {
        if (times-- <= 0)
            break;

        usleep(100000);

        kill(p, 0);
    } while (errno != ESRCH);

    if (times <= 0)
        throw string("Waited too long for task to exit");
}

static void WaitState(TPortoAPI &api, const string &name, const string &state) {
    cerr << "Waiting for " << name << " to be in state " << state << endl;

    int times = 100;

    string ret;
    do {
        if (times-- <= 0)
            break;

        usleep(100000);

        (void)api.GetData(name, "state", ret);
    } while (ret != state);

    if (times <= 0)
        throw string("Waited too long for task to change state");
}

static void WaitPortod(TPortoAPI &api) {
    cerr << "Waiting for portod startup" << endl;

    int times = 10;

    vector<string> clist;
    do {
        if (times-- <= 0)
            break;

        usleep(1000000);

    } while (api.List(clist) != 0);

    if (times <= 0)
        throw string("Waited too long for portod startup");
}

static string GetCwd(const string &pid) {
    string lnk;
    TFile f("/proc/" + pid + "/cwd");
    TError error(f.ReadLink(lnk));
    if (error)
        return error.GetMsg();
    return lnk;
}

static string GetNamespace(const string &pid, const string &ns) {
    string link;
    TFile m("/proc/" + pid + "/ns/" + ns);
    if (m.ReadLink(link))
        throw string("Can't get ") + ns + " namespace for " + pid;
    return link;
}

static map<string, string> GetCgroups(const string &pid) {
    TFile f("/proc/" + pid + "/cgroup");
    vector<string> lines;
    map<string, string> cgmap;
    if (f.AsLines(lines))
        throw string("Can't get cgroups");

    vector<string> tokens;
    for (auto l : lines) {
        tokens.clear();
        if (SplitString(l, ':', tokens))
            throw string("Can't split cgroup string");
        cgmap[tokens[1]] = tokens[2];
    }

    return cgmap;
}

static string GetStatusLine(const string &pid, const string &prefix) {
    vector<string> st;
    TFile f("/proc/" + pid + "/status");
    if (f.AsLines(st))
        throw string("INVALID PID");

    for (auto &s : st)
        if (s.substr(0, prefix.length()) == prefix)
            return s;

    throw string("INVALID PREFIX");
}

static string GetState(const string &pid) {
    stringstream ss(GetStatusLine(pid, "State:"));

    string name, state, desc;

    ss>> name;
    ss>> state;
    ss>> desc;

    if (name != "State:")
        throw string("PARSING ERROR");

    return state;
}

static void GetUidGid(const string &pid, int &uid, int &gid) {
    string name;
    string stuid = GetStatusLine(pid, "Uid:");
    stringstream ssuid(stuid);

    int euid, suid, fsuid;
    ssuid >> name;
    ssuid >> uid;
    ssuid >> euid;
    ssuid >> suid;
    ssuid >> fsuid;

    if (name != "Uid:" || uid != euid || euid != suid || suid != fsuid)
        throw string("Invalid uid");

    string stgid = GetStatusLine(pid, "Gid:");
    stringstream ssgid(stgid);

    int egid, sgid, fsgid;
    ssgid >> name;
    ssgid >> gid;
    ssgid >> egid;
    ssgid >> sgid;
    ssgid >> fsgid;

    if (name != "Gid:" || gid != egid || egid != sgid || sgid != fsgid)
        throw string("Invalid gid");
}

static int UserUid(const string &user) {
    struct passwd *p = getpwnam(user.c_str());
    if (!p)
        throw string("Invalid user");
    else
        return p->pw_uid;
}

static int GroupGid(const string &group) {
    struct group *g = getgrnam(group.c_str());
    if (!g)
        throw string("Invalid group");
    else
        return g->gr_gid;
}

static string GetEnv(const string &pid) {
    string env;
    TFile f("/proc/" + pid + "/environ");
    if (f.AsString(env))
        throw string("Can't get environment");

    return env;
}

static string CgRoot(const string &subsystem, const string &name) {
    return "/sys/fs/cgroup/" + subsystem + "/porto/" + name + "/";
}

static string GetFreezer(const string &name) {
    string link;
    TFile m(CgRoot("freezer", name) + "freezer.state");
    if (m.AsString(link))
        throw string("Can't get freezer");
    return link;
}

static void SetFreezer(const string &name, const string &state) {
    string link;
    TFile m(CgRoot("freezer", name) + "freezer.state");
    if (m.WriteStringNoAppend(state))
        throw string("Can't set freezer");

    int retries = 1000000;
    while (retries--)
        if (GetFreezer(name) == state + "\n")
            return;

    ExpectSuccess(-1);
}

static string GetCgKnob(const string &subsys, const string &name, const string &knob) {
    string val;
    TFile m(CgRoot(subsys, name) + knob);
    if (m.AsString(val))
        throw string("Can't get cgroup knob");
    val.erase(val.find('\n'));
    return val;
}

static bool HaveCgKnob(const string &subsys, const string &name, const string &knob) {
    string val;
    TFile m(CgRoot(subsys, name) + knob);
    return m.Exists();
}

static int GetVmRss(const string &pid) {
    stringstream ss(GetStatusLine(pid, "VmRSS:"));

    string name, size, unit;

    ss>> name;
    ss>> size;
    ss>> unit;

    if (name != "VmRSS:")
        throw string("PARSING ERROR");

    return stoi(size);
}

static string Pgrep(const string &name) {
    FILE *f = popen(("pgrep -x " + name).c_str(), "r");
    Expect(f != nullptr);

    char *line = nullptr;
    size_t n;

    string pid;
    int instances = 0;
    while (getline(&line, &n, f) >= 0) {
        pid.assign(line);
        pid.erase(pid.find('\n'));
        instances++;
    }

    Expect(instances == 1);
    fclose(f);

    return pid;
}

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
    ExpectSuccess(api.GetData(name, "parent", v));
    Expect(v == string("/"));
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
    ExpectFailure(api.Create("a"), EError::ContainerAlreadyExists);
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
    ExpectFailure(api.Start("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Stop("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Pause("a"), EError::ContainerDoesNotExist);
    ExpectFailure(api.Resume("a"), EError::ContainerDoesNotExist);

    string value;
    ExpectFailure(api.GetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.SetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectFailure(api.GetData("a", "root_pid", value), EError::ContainerDoesNotExist);

    cerr << "Try to create container with invalid name" << endl;
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

    cerr << "Test hierarchy" << endl;
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

    ExpectSuccess(api.Create("a/b/c"));
    containers.clear();
    ExpectSuccess(api.List(containers));
    Expect(containers.size() == 4);
    Expect(containers[0] == string("/"));
    Expect(containers[1] == string("a"));
    Expect(containers[2] == string("a/b"));
    Expect(containers[3] == string("a/b/c"));

    ExpectSuccess(api.Destroy("a/b/c"));
    ExpectSuccess(api.Destroy("a/b"));
    ExpectSuccess(api.Destroy("a"));

    ShouldHaveOnlyRoot(api);
}

static void TestEmpty(TPortoAPI &api) {
    cerr << "Make sure we can't start empty container" << endl;
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

static void TestExitStatus(TPortoAPI &api, const string &name) {
    string pid;
    string ret;

    cerr << "Check exit status of 'false'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "false"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("256"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    cerr << "Check exit status of 'true'" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("0"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));

    cerr << "Check exit status of invalid command" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("2"));

    cerr << "Check exit status of invalid directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "true"));
    ExpectSuccess(api.SetProperty(name, "cwd", "/__invalid__dir__"));
    ExpectFailure(api.Start(name), EError::Unknown);
    ExpectFailure(api.GetData(name, "root_pid", ret), EError::InvalidState);
    ExpectFailure(api.GetData(name, "exit_status", ret), EError::InvalidState);
    ExpectSuccess(api.GetData(name, "start_errno", ret));
    Expect(ret == string("-2"));

    cerr << "Check exit status when killed by signal" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "cwd", ""));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    kill(stoi(pid), SIGKILL);
    //Expect(TaskZombie(api, pid) == true);
    WaitState(api, name, "dead");
    ExpectSuccess(api.GetData(name, "exit_status", ret));
    Expect(ret == string("9"));
    ExpectFailure(api.GetData(name, "start_errno", ret), EError::InvalidState);
    ExpectSuccess(api.Stop(name));
}

static void TestStreams(TPortoAPI &api, const string &name) {
    string pid;
    string ret;

    cerr << "Make sure stdout works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("out\n"));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.Stop(name));

    cerr << "Make sure stderr works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string(""));
    ExpectSuccess(api.GetData(name, "stderr", ret));
    Expect(ret == string("err\n"));
    ExpectSuccess(api.Stop(name));
}

static void TestLongRunning(TPortoAPI &api, const string &name) {
    string pid;

    cerr << "Spawn long running task" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    Expect(TaskRunning(api, pid) == true);

    cerr << "Check that task namespaces are correct" << endl;
    Expect(GetNamespace("self", "pid") != GetNamespace(pid, "pid"));
    Expect(GetNamespace("self", "mnt") != GetNamespace(pid, "mnt"));
    Expect(GetNamespace("self", "ipc") == GetNamespace(pid, "ipc"));
    Expect(GetNamespace("self", "net") == GetNamespace(pid, "net"));
    //Expect(GetNamespace("self", "user") == GetNamespace(pid, "user"));
    Expect(GetNamespace("self", "uts") != GetNamespace(pid, "uts"));

    cerr << "Check that task cgroups are correct" << endl;
    auto cgmap = GetCgroups("self");
    for (auto name : cgmap) {
        Expect(name.second == "/");
    }

    ExpectCorrectCgroups(pid, name);

    ExpectSuccess(api.Stop(name));
    //Expect(TaskZombie(api, pid) == true);
    WaitExit(api, pid);
    Expect(TaskRunning(api, pid) == false);

    cerr << "Check that hierarchical task cgroups are correct" << endl;

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
}

static void TestIsolation(TPortoAPI &api, const string &name) {
    string ret;
    string pid;

    cerr << "Make sure PID isolation works" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);

    ExpectSuccess(api.GetData(name, "stdout", ret));
    Expect(ret == string("1\n"));
    ExpectSuccess(api.Stop(name));
}

static void TestEnvironment(TPortoAPI &api, const string &name) {
    string pid;

    cerr << "Check default environment" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    string env = GetEnv(pid);
    static const char empty_env[] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/home/nobody\0"
        "HOME=/home/nobody\0"
        "USER=nobody\0";

    Expect(memcmp(empty_env, env.data(), sizeof(empty_env)) == 0);
    ExpectSuccess(api.Stop(name));

    cerr << "Check user-defined environment" << endl;
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
}

static void TestUserGroup(TPortoAPI &api, const string &name) {
    int uid, gid;
    string pid;

    cerr << "Check default user & group" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("nobody"));
    Expect(gid == GroupGid("nogroup"));
    ExpectSuccess(api.Stop(name));

    cerr << "Check custom user & group" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.SetProperty(name, "user", "daemon"));
    ExpectSuccess(api.SetProperty(name, "group", "bin"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    Expect(uid == UserUid("daemon"));
    Expect(gid == GroupGid("bin"));
    ExpectSuccess(api.Stop(name));
}

static void TestCwd(TPortoAPI &api, const string &name) {
    string pid;
    string cwd;
    string portod_pid, portod_cwd;

    TFile portod(PID_FILE);
    (void)portod.AsString(portod_pid);
    portod_cwd = GetCwd(portod_pid);

    cerr << "Check default working directory" << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.GetData(name, "root_pid", pid));
    cwd = GetCwd(pid);

    Expect(cwd == portod_cwd);
    Expect(access(portod_cwd.c_str(), F_OK) == 0);
    ExpectSuccess(api.Stop(name));
    Expect(access(portod_cwd.c_str(), F_OK) == 0);

    cerr << "Check user defined working directory" << endl;
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
}

/*
static void TestRootProperty(TPortoAPI &api, const string &name) {
    string pid;

    cerr << "Check filesystem isolation" << endl;
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
}
*/

static void TestStateMachine(TPortoAPI &api, const string &name) {
    string pid;
    string v;

    cerr << "Check container state machine" << endl;

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
    //Expect(TaskZombie(api, pid) == true);
    WaitExit(api, pid);
    Expect(TaskRunning(api, pid) == false);

    cerr << "Make sure we can stop unintentionally frozen container " << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));

    v = GetFreezer(name);
    Expect(v == "THAWED\n");

    SetFreezer(name, "FROZEN");

    v = GetFreezer(name);
    Expect(v == "FROZEN\n");

    ExpectSuccess(api.Stop(name));

    cerr << "Make sure we can remove paused container " << endl;
    ExpectSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectSuccess(api.Start(name));
    ExpectSuccess(api.Pause(name));

    ExpectSuccess(api.Destroy(name));
}

static void TestRoot(TPortoAPI &api) {
    string v;
    string root = "/";
    vector<string> properties = { "command", "user", "group", "env", "memory_guarantee", "memory_limit", "cpu_policy", "cpu_priority", "parent", "respawn" };

    cerr << "Check root properties & data" << endl;
    for (auto p : properties)
        ExpectFailure(api.GetProperty(root, p, v), EError::InvalidProperty);

    ExpectSuccess(api.GetData(root, "state", v));
    Expect(v == string("running"));
    ExpectFailure(api.GetData(root, "exit_status", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "start_errno", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "root_pid", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "stdout", v), EError::InvalidData);
    ExpectFailure(api.GetData(root, "stderr", v), EError::InvalidData);

    ExpectFailure(api.Stop(root), EError::InvalidState);
    ExpectFailure(api.Destroy(root), EError::InvalidValue);

    cerr << "Check root cpu_usage & memory_usage" << endl;
    ExpectSuccess(api.GetData(root, "cpu_usage", v));
    Expect(v == "0");
    ExpectSuccess(api.GetData(root, "memory_usage", v));
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

    ExpectSuccess(api.Destroy(name));
}

static bool TestLimits(TPortoAPI &api, const string &name) {
    bool limitsTested = true;
    string pid;

    cerr << "Check default limits" << endl;
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
    } else {
        limitsTested = false;
    }
    ExpectSuccess(api.Stop(name));

    cerr << "Check custom limits" << endl;
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

    cerr << "Check cpu_priority" << endl;
    ExpectFailure(api.SetProperty(name, "cpu_priority", "-1"), EError::InvalidValue);
    ExpectFailure(api.SetProperty(name, "cpu_priority", "100"), EError::InvalidValue);
    ExpectSuccess(api.SetProperty(name, "cpu_priority", "0"));
    ExpectSuccess(api.SetProperty(name, "cpu_priority", "99"));
    // TODO: cpu_priority - check functionality when implemented

    cerr << "Check cpu_policy" << endl;
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
    } else {
        limitsTested = false;
    }
    // TODO: cpu_policy - check functionality when implemented

    return limitsTested;
}

static void TestLimitsHierarchy(TPortoAPI &api) {
    string pid;

    if (!HaveCgKnob("memory", "", "memory.low_limit_in_bytes"))
        return;

    cerr << "Check limits hierarchy" << endl;

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

    cerr << "Single container can't go over reserve" << endl;
    ExpectFailure(api.SetProperty(system, "memory_guarantee", to_string(total)), EError::ResourceNotAvailable);
    ExpectSuccess(api.SetProperty(system, "memory_guarantee", to_string(total - MEMORY_GUARANTEE_RESERVE)));

    cerr << "Distributed guarantee can't go over reserve" << endl;
    size_t chunk = (total - MEMORY_GUARANTEE_RESERVE) / 4;

    ExpectSuccess(api.SetProperty(system, "memory_guarantee", to_string(chunk)));
    ExpectSuccess(api.SetProperty(monit, "memory_guarantee", to_string(chunk)));
    ExpectSuccess(api.SetProperty(slot1, "memory_guarantee", to_string(chunk)));
    ExpectFailure(api.SetProperty(slot2, "memory_guarantee", to_string(chunk + 1)), EError::ResourceNotAvailable);
    ExpectSuccess(api.SetProperty(slot2, "memory_guarantee", to_string(chunk)));

    ExpectSuccess(api.SetProperty(monit, "memory_guarantee", to_string(0)));
    ExpectSuccess(api.SetProperty(system, "memory_guarantee", to_string(0)));

    auto CheckPropertyHierarhcy = [&](TPortoAPI &api, const std::string &property) {
        cerr << "Parent can't have less guarantee than sum of children" << endl;
        ExpectSuccess(api.SetProperty(slot1, property, to_string(chunk)));
        ExpectSuccess(api.SetProperty(slot2, property, to_string(chunk)));
        ExpectFailure(api.SetProperty(prod, property, to_string(chunk)), EError::InvalidValue);
        ExpectFailure(api.SetProperty(box, property, to_string(chunk)), EError::InvalidValue);

        cerr << "Child can't go over parent guarantee" << endl;
        ExpectSuccess(api.SetProperty(prod, property, to_string(2 * chunk)));
        ExpectFailure(api.SetProperty(slot1, property, to_string(2 * chunk)), EError::InvalidValue);

        cerr << "Can lower guarantee if possible" << endl;
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

static void TestPermissions(TPortoAPI &api, const string &name) {
    struct stat st;
    string path;

    cerr << "Check permissions" << endl;

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
}

static void TestRespawn(TPortoAPI &api, const string &name) {
    string pid, respawnPid;

    cerr << "Check respawn" << endl;

    ExpectSuccess(api.SetProperty(name, "command", "sleep 1"));
    ExpectSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectSuccess(api.Start(name));

    ExpectSuccess(api.GetData(name, "root_pid", pid));
    WaitExit(api, pid);
    ExpectSuccess(api.GetData(name, "root_pid", respawnPid));
    Expect(pid != respawnPid);

    ExpectSuccess(api.Stop(name));
    ExpectSuccess(api.SetProperty(name, "respawn", "false"));
}

static void TestLeaks(TPortoAPI &api) {
    string pid;
    string name;
    int nr = 1000;
    int slack = 4096;

    TFile f(PID_FILE);
    Expect(f.AsString(pid) == false);

    cerr << "Check daemon leaks" << endl;

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

static void TestDaemon() {
    struct dirent **lst;
    size_t n;
    string pid;

    cerr << "Make sure portod doesn't have zombies" << endl;
    pid = Pgrep("portod");

    cerr << "Make sure portod doesn't have invalid FDs" << endl;
    string path = ("/proc/" + pid + "/fd");
    n = scandir(path.c_str(), &lst, NULL, alphasort);
    // . .. 0(stdin) 1(stdout) 2(stderr) 4(log) 5(rpc socket) 128(event pipe) 129(ack pipe)
    Expect(n == 2 + 7);

    cerr << "Make sure portoloop doesn't have zombies" << endl;
    pid = Pgrep("portoloop");

    cerr << "Make sure portoloop doesn't have invalid FDs" << endl;
    path = ("/proc/" + pid + "/fd");
    n = scandir(path.c_str(), &lst, NULL, alphasort);
    // . .. 0(stdin) 1(stdout) 2(stderr) 3(log) 128(event pipe) 129(ack pipe)
    Expect(n == 2 + 6);
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

    cerr << "Make sure we don't kill containers when doing recovery" << endl;
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

    cerr << "Make sure hierarchical recovery works" << endl;

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

int Selftest() {
    // TODO: truncate portoloop log and check that we don't have unexpected
    // respawns

    bool limitsTested = false;

    try {
        {
            TPortoAPI api;
            TestRoot(api);
            TestHolder(api);
            TestEmpty(api);
            TestStateMachine(api, "a");

            ExpectSuccess(api.Create("a"));
            TestExitStatus(api, "a");
            TestStreams(api, "a");
            TestLongRunning(api, "a");
            TestIsolation(api, "a");
            TestEnvironment(api, "a");
            TestUserGroup(api, "a");
            TestCwd(api, "a");
            //TestRootProperty(api, "a");
            limitsTested = TestLimits(api, "a");
            TestPermissions(api, "a");
            TestRespawn(api, "a");
            ExpectSuccess(api.Destroy("a"));

            TestLimitsHierarchy(api);
            TestLeaks(api);
        }
        TestDaemon();
        {
            TPortoAPI api;
            TestRecovery(api);
        }
    } catch (string e) {
        cerr << "EXCEPTION: " << e << endl;
        return 1;
    }

    cerr << "All tests successfully passed!" << endl;
    if (!limitsTested)
        cerr << "WARNING: Due to missing kernel support, memory_guarantee/cpu_policy has not been tested!" << endl;

    return 0;
}
