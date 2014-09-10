#include <sstream>

#include "test.hpp"
#include "util/string.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <pwd.h>
#include <dirent.h>
}

namespace Test {

__thread int tid;
std::atomic<int> done;

std::basic_ostream<char> &Say(std::basic_ostream<char> &stream) {
    if (tid)
        return stream << "[" << tid << "] ";
    else
        return std::cerr << "- ";
}

void ExpectReturn(std::function<int()> f, int exp, int retry, int line, const char *func) {
    int ret;
    while (retry--) {
        if (done)
            throw std::string("stop thread.");
        ret = f();
        if (ret == exp)
            return;
        usleep(1000000);
        Say(std::cerr) << "Retry " << func << ":" << line << " Ret=" << ret << " " << std::endl;
    }
    done++;
    throw std::string("Got " + std::to_string(ret) + ", but expected " + std::to_string(exp) + " at " + func + ":" + std::to_string(line));
}

std::string Pgrep(const std::string &name) {
    FILE *f = popen(("pgrep -x " + name).c_str(), "r");
    if (f == nullptr)
        throw std::string("Can't execute pgrep");

    char *line = nullptr;
    size_t n;

    std::string pid;
    int instances = 0;
    while (getline(&line, &n, f) >= 0) {
        pid.assign(line);
        pid.erase(pid.find('\n'));
        instances++;
    }

    if (instances != 1)
        throw std::string("pgrep returned several instances");
    fclose(f);

    return pid;
}

void WaitExit(TPortoAPI &api, const std::string &pid) {
    Say() << "Waiting for " << pid << " to exit..." << std::endl;

    int times = 100;
    int p = stoi(pid);

    do {
        if (times-- <= 0)
            break;

        usleep(100000);

        kill(p, 0);
    } while (errno != ESRCH);

    if (times <= 0)
        throw std::string("Waited too long for task to exit");
}

void WaitState(TPortoAPI &api, const std::string &name, const std::string &state) {
    Say() << "Waiting for " << name << " to be in state " << state << std::endl;

    int times = 100;

    std::string ret;
    do {
        if (times-- <= 0)
            break;

        usleep(100000);

        (void)api.GetData(name, "state", ret);
    } while (ret != state);

    if (times <= 0)
        throw std::string("Waited too long for task to change state");
}

void WaitPortod(TPortoAPI &api) {
    Say() << "Waiting for portod startup" << std::endl;

    int times = 10;

    std::vector<std::string> clist;
    do {
        if (times-- <= 0)
            break;

        usleep(1000000);

    } while (api.List(clist) != 0);

    if (times <= 0)
        throw std::string("Waited too long for portod startup");
}

std::string GetCwd(const std::string &pid) {
    std::string lnk;
    TFile f("/proc/" + pid + "/cwd");
    TError error(f.ReadLink(lnk));
    if (error)
        return error.GetMsg();
    return lnk;
}

std::string GetNamespace(const std::string &pid, const std::string &ns) {
    std::string link;
    TFile m("/proc/" + pid + "/ns/" + ns);
    if (m.ReadLink(link))
        throw std::string("Can't get ") + ns + " namespace for " + pid;
    return link;
}

std::map<std::string, std::string> GetCgroups(const std::string &pid) {
    TFile f("/proc/" + pid + "/cgroup");
    std::vector<std::string> lines;
    std::map<std::string, std::string> cgmap;
    if (f.AsLines(lines))
        throw std::string("Can't get cgroups");

    std::vector<std::string> tokens;
    for (auto l : lines) {
        tokens.clear();
        if (SplitString(l, ':', tokens))
            throw std::string("Can't split cgroup string");
        cgmap[tokens[1]] = tokens[2];
    }

    return cgmap;
}

std::string GetStatusLine(const std::string &pid, const std::string &prefix) {
    std::vector<std::string> st;
    TFile f("/proc/" + pid + "/status");
    if (f.AsLines(st))
        throw std::string("INVALID PID");

    for (auto &s : st)
        if (s.substr(0, prefix.length()) == prefix)
            return s;

    throw std::string("INVALID PREFIX");
}

std::string GetState(const std::string &pid) {
    std::stringstream ss(GetStatusLine(pid, "State:"));

    std::string name, state, desc;

    ss>> name;
    ss>> state;
    ss>> desc;

    if (name != "State:")
        throw std::string("PARSING ERROR");

    return state;
}

void GetUidGid(const std::string &pid, int &uid, int &gid) {
    std::string name;
    std::string stuid = GetStatusLine(pid, "Uid:");
    std::stringstream ssuid(stuid);

    int euid, suid, fsuid;
    ssuid >> name;
    ssuid >> uid;
    ssuid >> euid;
    ssuid >> suid;
    ssuid >> fsuid;

    if (name != "Uid:" || uid != euid || euid != suid || suid != fsuid)
        throw std::string("Invalid uid");

    std::string stgid = GetStatusLine(pid, "Gid:");
    std::stringstream ssgid(stgid);

    int egid, sgid, fsgid;
    ssgid >> name;
    ssgid >> gid;
    ssgid >> egid;
    ssgid >> sgid;
    ssgid >> fsgid;

    if (name != "Gid:" || gid != egid || egid != sgid || sgid != fsgid)
        throw std::string("Invalid gid");
}

int UserUid(const std::string &user) {
    struct passwd *p = getpwnam(user.c_str());
    if (!p)
        throw std::string("Invalid user");
    else
        return p->pw_uid;
}

int GroupGid(const std::string &group) {
    struct group *g = getgrnam(group.c_str());
    if (!g)
        throw std::string("Invalid group");
    else
        return g->gr_gid;
}

std::string GetEnv(const std::string &pid) {
    std::string env;
    TFile f("/proc/" + pid + "/environ");
    if (f.AsString(env))
        throw std::string("Can't get environment");

    return env;
}

bool CgExists(const std::string &subsystem, const std::string &name) {
    TFile f(CgRoot(subsystem, name));
    return f.Exists();
}

std::string CgRoot(const std::string &subsystem, const std::string &name) {
    return "/sys/fs/cgroup/" + subsystem + "/porto/" + name + "/";
}

std::string GetFreezer(const std::string &name) {
    std::string link;
    TFile m(CgRoot("freezer", name) + "freezer.state");
    if (m.AsString(link))
        throw std::string("Can't get freezer");
    return link;
}

void SetFreezer(const std::string &name, const std::string &state) {
    std::string link;
    TFile m(CgRoot("freezer", name) + "freezer.state");
    if (m.WriteStringNoAppend(state))
        throw std::string("Can't set freezer");

    int retries = 1000000;
    while (retries--)
        if (GetFreezer(name) == state + "\n")
            return;

    throw std::string("Can't set freezer state to ") + state;
}

std::string GetCgKnob(const std::string &subsys, const std::string &name, const std::string &knob) {
    std::string val;
    TFile m(CgRoot(subsys, name) + knob);
    if (m.AsString(val))
        throw std::string("Can't get cgroup knob " + m.GetPath());
    val.erase(val.find('\n'));
    return val;
}

bool HaveCgKnob(const std::string &subsys, const std::string &knob) {
    std::string val;
    TFile m(CgRoot(subsys, "") + knob);
    return m.Exists();
}

int GetVmRss(const std::string &pid) {
    std::stringstream ss(GetStatusLine(pid, "VmRSS:"));

    std::string name, size, unit;

    ss>> name;
    ss>> size;
    ss>> unit;

    if (name != "VmRSS:")
        throw std::string("PARSING ERROR");

    return std::stoi(size);
}

void TestDaemon(TPortoAPI &api) {
    struct dirent **lst;
    size_t n;
    std::string pid;

    api.Cleanup();

    Say() << "Make sure portod doesn't have zombies" << std::endl;
    pid = Pgrep("portod");

    Say() << "Make sure portod doesn't have invalid FDs" << std::endl;
    std::string path = ("/proc/" + pid + "/fd");
    n = scandir(path.c_str(), &lst, NULL, alphasort);
    // . .. 0(stdin) 1(stdout) 2(stderr) 4(log) 5(rpc socket) 128(event pipe) 129(ack pipe)
    if (n != 2 + 7)
        throw std::string("Invalid number of fds");

    Say() << "Make sure portoloop doesn't have zombies" << std::endl;
    pid = Pgrep("portoloop");

    Say() << "Make sure portoloop doesn't have invalid FDs" << std::endl;
    path = ("/proc/" + pid + "/fd");
    n = scandir(path.c_str(), &lst, NULL, alphasort);
    // . .. 0(stdin) 1(stdout) 2(stderr) 3(log) 128(event pipe) 129(ack pipe)
    if (n != 2 + 6)
        throw std::string("Invalid number of fds");
}
}
