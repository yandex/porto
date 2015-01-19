#ifndef __TEST_H__
#define __TEST_H__

#include <iostream>
#include <string>
#include <atomic>

#include "libporto.hpp"
#include "util/pwd.hpp"

class TNlLink;

namespace test {
    extern __thread int tid;
    extern std::atomic<int> done;
    extern std::vector<std::shared_ptr<TNlLink>> links;

    std::basic_ostream<char> &Say(std::basic_ostream<char> &stream = std::cout);
    void ExpectReturn(int ret, int exp, int line, const char *func);

    int ReadPid(const std::string &path);
    std::vector<std::string> Popen(const std::string &cmd);
    int Pgrep(const std::string &name);
    std::string GetRlimit(const std::string &pid, const std::string &type, const bool soft);
    void WaitExit(TPortoAPI &api, const std::string &pid);
    void WaitState(TPortoAPI &api, const std::string &name, const std::string &state, int sec = 10);
    void WaitPortod(TPortoAPI &api, int times = 10);
    std::string GetCwd(const std::string &pid);
    std::string GetRoot(const std::string &pid);
    std::string GetNamespace(const std::string &pid, const std::string &ns);
    std::map<std::string, std::string> GetCgroups(const std::string &pid);
    std::string GetStatusLine(const std::string &pid, const std::string &prefix);
    std::string GetState(const std::string &pid);
    uint64_t GetCap(const std::string &pid, const std::string &type);
    void GetUidGid(const std::string &pid, int &uid, int &gid);
    int UserUid(const std::string &user);
    int GroupGid(const std::string &group);
    std::string GetEnv(const std::string &pid);
    bool CgExists(const std::string &subsystem, const std::string &name);
    std::string CgRoot(const std::string &subsystem, const std::string &name);
    std::string GetFreezer(const std::string &name);
    void SetFreezer(const std::string &name, const std::string &state);
    std::string GetCgKnob(const std::string &subsys, const std::string &name, const std::string &knob);
    bool HaveCgKnob(const std::string &subsys, const std::string &knob);
    int GetVmRss(const std::string &pid);
    bool TcClassExist(uint32_t handle);
    bool TcQdiscExist(uint32_t handle);
    bool TcCgFilterExist(uint32_t parent, uint32_t handle);
    int WordCount(const std::string &path, const std::string &word);
    std::string ReadLink(const std::string &path);
    bool FileExists(const std::string &path);
    void AsUser(TPortoAPI &api, TUser &user, TGroup &group);
    void AsRoot(TPortoAPI &api);
    void AsNobody(TPortoAPI &api);
    void AsDaemon(TPortoAPI &api);
    std::string GetDefaultUser();
    std::string GetDefaultGroup();
    void BootstrapCommand(const std::string &cmd, const std::string &path, bool remove = true);

    void RestartDaemon(TPortoAPI &api);
    bool NetworkEnabled();
    void TestDaemon(TPortoAPI &api);

    int SelfTest(std::string name, int leakNr);
    int StressTest(int threads, int iter, bool killPorto);

    bool IsCfqActive();
}

#define Expect(ret) ExpectReturn(ret, true, __LINE__, __func__)
#define ExpectSuccess(ret) ExpectReturn(ret, 0, __LINE__, __func__)
#define ExpectFailure(ret, exp) ExpectReturn(ret, exp, __LINE__, __func__)

#endif
