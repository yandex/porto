#pragma once

#include <string>
#include <vector>
#include <list>
#include <memory>

#include "util/unix.hpp"
#include "util/locks.hpp"
#include "util/log.hpp"
#include "util/idmap.hpp"
#include "stream.hpp"
#include "cgroup.hpp"
#include "property.hpp"

class TEpollSource;
class TCgroup;
class TSubsystem;
class TEvent;
enum class ENetStat;
class TNetwork;
class TNamespaceFd;
class TContainerWaiter;
class TClient;
class TVolume;
class TKeyValue;
struct TBindMount;

struct TEnv;

enum class EContainerState {
    Stopped,
    Dead,
    Running,
    Paused,
    Meta,
    Destroyed,
};

class TProperty;

class TContainer : public std::enable_shared_from_this<TContainer>,
                   public TNonCopyable {
    friend class TProperty;

    int Locked = 0;

    TFile OomEvent;

    /* protected with ContainersMutex */
    std::list<std::weak_ptr<TContainerWaiter>> Waiters;

    std::shared_ptr<TEpollSource> Source;

    // data
    TError UpdateSoftLimit();
    void SetState(EContainerState next);

    TError ApplyDynamicProperties();
    TError PrepareWorkDir();
    TError RestoreNetwork();
    TError PrepareOomMonitor();
    void ShutdownOom();
    TError PrepareCgroups();
    TError ConfigureDevices(std::vector<TDevice> &devices);
    TError ParseNetConfig(struct TNetCfg &NetCfg);
    TError PrepareNetwork(struct TNetCfg &NetCfg);
    TError PrepareTask(struct TTaskEnv *TaskEnv,
                       struct TNetCfg *NetCfg);

    void ScheduleRespawn();
    TError Respawn();
    TError PrepareResources();
    void FreeResources();

    void Reap(bool oomKilled);
    void Exit(int status, bool oomKilled);

    void CleanupWaiters();
    void NotifyWaiters();

    TError CallPostorder(std::function<TError (TContainer &ct)> fn);

public:
    const std::shared_ptr<TContainer> Parent;
    const std::string Name;
    const std::string FirstName;
    const int Level; // 0 for root

    int Id = 0;

    /* protected with exclusive lock and ContainersMutex */
    EContainerState State = EContainerState::Stopped;
    int RunningChildren = 0;

    bool PropSet[(int)EProperty::NR_PROPERTIES];
    bool PropDirty[(int)EProperty::NR_PROPERTIES];
    uint64_t Controllers, RequiredControllers;
    TCred OwnerCred;
    std::string Command;
    std::string Cwd;
    TStdStream Stdin, Stdout, Stderr;
    std::string Root;
    bool RootRo;
    mode_t Umask;
    int VirtMode;
    bool BindDns;
    bool Isolate;
    std::vector<std::string> NetProp;
    std::list<std::shared_ptr<TContainer>> Children;
    std::string Hostname;
    std::vector<std::string> EnvCfg;
    std::vector<TBindMount> BindMounts;
    std::vector<std::string> IpList;
    TCapabilities CapAmbient;   /* get at start */
    TCapabilities CapAllowed;   /* can set as ambient */
    TCapabilities CapLimit;     /* can get by suid */
    std::vector<std::string> DefaultGw;
    std::vector<std::string> ResolvConf;
    std::vector<std::string> Devices;

    uint64_t StartTime;
    uint64_t DeathTime;
    uint64_t AgingTime;

    std::map<int, struct rlimit> Rlimit;
    std::string NsName;

    uint64_t MemLimit = 0;
    uint64_t MemGuarantee = 0;
    uint64_t NewMemGuarantee = 0;
    uint64_t AnonMemLimit = 0;
    uint64_t DirtyMemLimit = 0;
    int64_t HugetlbLimit = -1;

    bool RechargeOnPgfault = false;

    std::string IoPolicy;
    uint64_t IoLimit = 0;
    uint64_t IopsLimit = 0;

    std::string CpuPolicy;
    double CpuLimit;
    double CpuGuarantee;

    TUintMap NetGuarantee;
    TUintMap NetLimit;
    TUintMap NetPriority;

    bool ToRespawn;
    int MaxRespawns;
    uint64_t RespawnCount;

    std::string Private;
    EAccessLevel AccessLevel;

    bool IsWeak = false;
    bool OomKilled = false;
    int ExitStatus = 0;

    TPath RootPath; /* path in host namespace, set at start */
    int LoopDev = -1; /* legacy */
    std::shared_ptr<TVolume> RootVolume;

    TTask Task;
    pid_t TaskVPid;
    TTask WaitTask;
    std::shared_ptr<TNetwork> Net;

    std::string GetCwd() const;
    TPath WorkPath() const;

    bool IsMeta() const {
        return Command.empty();
    }

    TContainer(std::shared_ptr<TContainer> parent, const std::string &name);
    ~TContainer();

    void Register();

    bool HasProp(EProperty prop) const {
        return PropSet[(int)prop];
    }

    void SetProp(EProperty prop) {
        PropSet[(int)prop] = true;
        PropDirty[(int)prop] = true;
    }

    void ClearProp(EProperty prop) {
        PropSet[(int)prop] = false;
        PropDirty[(int)prop] = true;
    }

    bool TestClearPropDirty(EProperty prop) {
        if (!PropDirty[(int)prop])
            return false;
        PropDirty[(int)prop] = false;
        return true;
    }

    std::string GetPortoNamespace() const;

    TError Lock(TScopedLock &lock, bool shared = false, bool try_lock = false);
    TError LockRead(TScopedLock &lock, bool try_lock = false) {
        return Lock(lock, true, try_lock);
    }
    void Unlock(bool locked = false);

    void SanitizeCapabilities();
    uint64_t GetTotalMemGuarantee(void) const;
    uint64_t GetTotalMemLimit(const TContainer *base = nullptr) const;
    bool IsolatedFromHost() const;

    bool IsRoot() const { return !Level; }
    bool IsChildOf(const TContainer &ct) const;

    std::list<std::shared_ptr<TContainer>> Subtree();

    std::shared_ptr<TContainer> GetParent() const;
    TError OpenNetns(TNamespaceFd &netns) const;

    TError GetNetStat(ENetStat kind, TUintMap &stat);
    uint32_t GetTrafficClass() const;

    pid_t GetPidFor(pid_t pid) const;

    TError StartTask();
    TError Start();
    TError Stop(uint64_t timeout);
    TError Pause();
    TError Resume();
    TError Terminate(uint64_t deadline);
    TError Kill(int sig);
    TError Destroy();

    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value);

    void ForgetPid();
    void SyncState();
    bool Expired() const;
    void DestroyWeak();

    TError Save(void);
    TError Load(const TKeyValue &node);

    TCgroup GetCgroup(const TSubsystem &subsystem) const;
    std::shared_ptr<TContainer> FindRunningParent() const;

    void AddWaiter(std::shared_ptr<TContainerWaiter> waiter);

    TError UpdateTrafficClasses();

    bool MayRespawn();
    bool MayReceiveOom(int fd);
    bool HasOomReceived();

    /* protected with VolumesLock */
    std::list<std::shared_ptr<TVolume>> Volumes;

    TError GetEnvironment(TEnv &env);

    static TError ValidName(const std::string &name);
    static std::string ParentName(const std::string &name);

    static std::string StateName(EContainerState state);

    static std::shared_ptr<TContainer> Find(const std::string &name);
    static TError Find(const std::string &name, std::shared_ptr<TContainer> &ct);
    static TError FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &ct);

    static TError Create(const std::string &name, std::shared_ptr<TContainer> &ct);
    static TError Restore(const TKeyValue &kv, std::shared_ptr<TContainer> &ct);

    static void Event(const TEvent &event);
};

class TContainerWaiter {
private:
    static std::mutex WildcardLock;
    static std::list<std::weak_ptr<TContainerWaiter>> WildcardWaiters;
    std::weak_ptr<TClient> Client;
    std::function<void (std::shared_ptr<TClient>, TError, std::string)> Callback;
public:
    TContainerWaiter(std::shared_ptr<TClient> client,
                     std::function<void (std::shared_ptr<TClient>, TError, std::string)> callback);
    void WakeupWaiter(const TContainer *who, bool wildcard = false);
    static void WakeupWildcard(const TContainer *who);
    static void AddWildcard(std::shared_ptr<TContainerWaiter> &waiter);

    std::vector<std::string> Wildcards;
    bool MatchWildcard(const std::string &name);
};

extern std::mutex ContainersMutex;
extern std::shared_ptr<TContainer> RootContainer;
extern std::map<std::string, std::shared_ptr<TContainer>> Containers;
extern TPath ContainersKV;
extern TIdMap ContainerIdMap;

static inline std::unique_lock<std::mutex> LockContainers() {
    return std::unique_lock<std::mutex>(ContainersMutex);
}
