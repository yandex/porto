#ifndef __TEST_H__
#define __TEST_H__

#include <iostream>
#include <string>
#include <atomic>

#include "libporto.hpp"

namespace Test {
    extern __thread int tid;
    extern std::atomic<int> done;

    std::basic_ostream<char> &Say(std::basic_ostream<char> &stream = std::cout);
    void ExpectReturn(std::function<int()> f, int exp, int retry, int line, const char *func);
    void TestDaemon(TPortoAPI &api);
    std::string Pgrep(const std::string &name);

    void WaitExit(TPortoAPI &api, const std::string &pid);
    void WaitState(TPortoAPI &api, const std::string &name, const std::string &state);
    void WaitPortod(TPortoAPI &api);
    std::string GetCwd(const std::string &pid);
    std::string GetNamespace(const std::string &pid, const std::string &ns);
    std::map<std::string, std::string> GetCgroups(const std::string &pid);
    std::string GetStatusLine(const std::string &pid, const std::string &prefix);
    std::string GetState(const std::string &pid);
    void GetUidGid(const std::string &pid, int &uid, int &gid);
    int UserUid(const std::string &user);
    int GroupGid(const std::string &group);
    std::string GetEnv(const std::string &pid);
    std::string CgRoot(const std::string &subsystem, const std::string &name);
    std::string GetFreezer(const std::string &name);
    void SetFreezer(const std::string &name, const std::string &state);
    std::string GetCgKnob(const std::string &subsys, const std::string &name, const std::string &knob);
    bool HaveCgKnob(const std::string &subsys, const std::string &name, const std::string &knob);
    int GetVmRss(const std::string &pid);
    int SelfTest(std::string name);
    int StressTest(int threads, int iter, bool killPorto);
}

#endif
