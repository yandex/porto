#pragma once

#include <iostream>
#include <string>
#include <atomic>

#include "libporto.hpp"
#include "util/cred.hpp"

extern "C" {
#include <dirent.h>
}

class TNlLink;

namespace test {
    extern __thread int tid;
    extern std::atomic<int> done;
    extern std::vector<std::shared_ptr<TNlLink>> links;

    std::basic_ostream<char> &Say(std::basic_ostream<char> &stream = std::cout);
    void ExpectReturn(int ret, int exp, int line, const char *func);
    void ExpectError(const TError &ret, const TError &exp, int line, const char *func);
    void ExpectApi(TPortoAPI &api, int ret, int exp, int line, const char *func);

    int ReadPid(const std::string &path);
    int Pgrep(const std::string &name);
    std::string GetRlimit(const std::string &pid, const std::string &type, const bool soft);
    void WaitProcessExit(const std::string &pid, int sec = 10);
    void WaitState(TPortoAPI &api, const std::string &name, const std::string &state, int sec = 10);
    void WaitContainer(TPortoAPI &api, const std::string &name, int sec = 10);
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

    void RotateDaemonLogs(TPortoAPI &api);
    void RestartDaemon(TPortoAPI &api);
    void PrintFds(const std::string &path, struct dirent **lst, int nr);
    bool NetworkEnabled();
    void TestDaemon(TPortoAPI &api);

    int SelfTest(std::vector<std::string> name, int leakNr);
    int StressTest(int threads, int iter, bool killPorto);
    int FuzzyTest(int threads, int iter);

    enum class KernelFeature {
        SMART,
        LOW_LIMIT,
        RECHARGE_ON_PGFAULT,
        FSIO,
        CFS_BANDWIDTH,
        CFS_GROUPSCHED,
        IPVLAN,
        MAX_RSS,
        CFQ,
        LAST
    };

    void InitKernelFeatures();
    bool KernelSupports(const KernelFeature &feature);

    void _ExpectEq(size_t ret, size_t exp, size_t line, const char *func);
    void _ExpectEq(const std::string &ret, const std::string &exp, size_t line, const char *func);
    void _ExpectNeq(size_t ret, size_t exp, size_t line, const char *func);
    void _ExpectNeq(const std::string &ret, const std::string &exp, size_t line, const char *func);
    void _ExpectLess(size_t ret, size_t exp, size_t line, const char *func);
    void _ExpectLess(const std::string &ret, const std::string &exp, size_t line, const char *func);
}

#define Expect(ret) ExpectReturn(ret, true, __LINE__, __func__)

#define ExpectSuccess(ret) ExpectError(ret, TError::Success(), __LINE__, __func__)
#define ExpectFailure(ret, exp) ExpectError(ret, exp, __LINE__, __func__)

#define ExpectApiSuccess(ret) ExpectApi(api, ret, 0, __LINE__, __func__)
#define ExpectApiFailure(ret, exp) ExpectApi(api, ret, exp, __LINE__, __func__)

#define ExpectEq(ret, exp) _ExpectEq(ret, exp, __LINE__, __func__)
#define ExpectNeq(ret, exp) _ExpectNeq(ret, exp, __LINE__, __func__)
#define ExpectLess(ret, exp) _ExpectLess(ret, exp, __LINE__, __func__)
