#pragma once

#include <string>
#include <vector>
#include <list>
#include <memory>

#include "util/unix.hpp"
#include "util/locks.hpp"
#include "util/log.hpp"
#include "stream.hpp"
#include "cgroup.hpp"
#include "task.hpp"

class TKeyValueStorage;
class TEpollSource;
class TCgroup;
class TSubsystem;
class TPropertyMap;
class TValueMap;
enum class ETclassStat;
class TEvent;
class TContainerHolder;
class TNetwork;
class TNamespaceFd;
class TNlLink;
class TTask;
class TContainerWaiter;
class TClient;
class TVolume;
class TVolumeHolder;

namespace kv {
    class TNode;
};

struct TEnv;

enum class EContainerState {
    Unknown,
    Stopped,
    Dead,
    Running,
    Paused,
    Meta
};

class TContainerProperty;
class TContainerPropertyMap;

class TContainer : public std::enable_shared_from_this<TContainer>,
                   public TNonCopyable,
                   public TLockable {
    friend class TContainerProperty;

    std::shared_ptr<TContainerHolder> Holder;
    const std::string Name;
    const std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TKeyValueStorage> Storage;
    int Acquired = 0;
    int Id;
    int TaskStartErrno = -1;
    TScopedFd OomEventFd;
    size_t CgroupEmptySince = 0;
    size_t RunningChildren = 0; // changed under holder lock
    bool LostAndRestored = false;
    std::list<std::weak_ptr<TContainerWaiter>> Waiters;

    std::shared_ptr<TEpollSource> Source;
    bool IsMeta = false;

    TStdStream Stdin, Stdout, Stderr;
    int Level; // 0 for root, 1 for porto_root, etc

    // data
    void UpdateRunningChildren(size_t diff);
    TError UpdateSoftLimit();
    void SetState(EContainerState newState);

    TError ApplyDynamicProperties();
    TError PrepareWorkDir();
    TError RestoreNetwork();
    TError PrepareOomMonitor();
    TError PrepareLoop();
    void ShutdownOom();
    TError PrepareCgroups();
    TError ConfigureDevices(std::vector<TDevice> &devices);
    TError ParseNetConfig(struct TNetCfg &NetCfg);
    TError PrepareNetwork(struct TNetCfg &NetCfg);
    TError ConfigureNetwork(struct TNetCfg &NetCfg);
    TError PrepareTask(std::shared_ptr<TClient> client, struct TNetCfg *NetCfg);
    TError KillAll(TScopedLock &holder_lock);
    void RemoveKvs();

    const std::string StripParentName(const std::string &name) const;
    void ScheduleRespawn();
    TError Respawn(TScopedLock &holder_lock);
    void StopChildren(TScopedLock &holder_lock);
    TError PrepareResources(std::shared_ptr<TClient> client);
    void FreeResources();

    void RestoreStdPath(const std::string &property,
                        const std::string &path, bool is_default);
    void CreateStdStreams();
    TError PrepareStdStreams(std::shared_ptr<TClient> client);

    void ExitTree(TScopedLock &holder_lock, int status, bool oomKilled);
    void Exit(TScopedLock &holder_lock, int status, bool oomKilled);

    TError Prepare();

    void CleanupWaiters();
    void NotifyWaiters();

    // fn called for parent first then for all children (from top container to the leafs)
    TError ApplyForTreePreorder(TScopedLock &holder_lock,
                                std::function<TError (TScopedLock &holder_lock,
                                                      TContainer &container)> fn);
    // fn called for children first then for all parents (from leaf containers to the top)
    TError ApplyForTreePostorder(TScopedLock &holder_lock,
                                 std::function<TError (TScopedLock &holder_lock,
                                                       TContainer &container)> fn);

    void DestroyVolumes(TScopedLock &holder_lock);

    TError Unfreeze(TScopedLock &holder_lock);
    TError Freeze(TScopedLock &holder_lock);

public:
    uint64_t PropMask;
    TCred OwnerCred;
    uint64_t MemGuarantee;
    uint64_t CurrentMemGuarantee;
    std::string Command;
    std::string Cwd;
    std::string StdinPath;
    std::string StdoutPath;
    std::string StderrPath;
    std::string Root;
    bool RootRo;
    int VirtMode = 0;
    bool BindDns;
    bool Isolate;
    std::vector<std::string> NetProp;
    std::vector<std::weak_ptr<TContainer>> Children;
    std::string Hostname;
    std::vector<std::string> EnvCfg;
    std::vector<TBindMap> BindMap;
    std::vector<std::string> IpList;
    uint64_t Caps;
    std::vector<std::string> DefaultGw;
    std::vector<std::string> ResolvConf;
    std::vector<std::string> Devices;
    std::vector<int> RootPid;
    int LoopDev;
    uint64_t StartTime;
    uint64_t DeathTime;
    std::map<int, struct rlimit> Rlimit;
    std::string NsName;
    uint64_t StdoutLimit;
    uint64_t MemLimit;
    uint64_t AnonMemLimit;
    uint64_t DirtyMemLimit;
    bool RechargeOnPgfault;
    std::string CpuPolicy;
    double CpuLimit;
    double CpuGuarantee;
    std::string IoPolicy;
    uint64_t IoLimit;
    uint64_t IopsLimit;
    TUintMap NetGuarantee;
    TUintMap NetLimit;
    TUintMap NetPriority;
    bool ToRespawn;
    int MaxRespawns;
    uint64_t RespawnCount;
    std::string Private;
    uint64_t AgingTime;
    bool PortoEnabled;
    bool IsWeak;
    EContainerState State = EContainerState::Unknown;
    bool OomKilled;
    int ExitStatus;

    // TODO: make private
    std::unique_ptr<TTask> Task;
    std::shared_ptr<TPropertyMap> Prop;
    std::shared_ptr<TValueMap> Data;
    std::shared_ptr<TNetwork> Net;

    TPath GetTmpDir() const;
    TPath RootPath() const;
    TPath WorkPath() const;
    TPath ActualStdPath(const std::string &path_str, bool is_default, bool host) const;
    TError RotateStdFile(TStdStream &stream, const std::string &type);
    EContainerState GetState() const;
    TError GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m);

    TContainer(std::shared_ptr<TContainerHolder> holder,
               std::shared_ptr<TKeyValueStorage> storage,
               const std::string &name, std::shared_ptr<TContainer> parent,
               int id);
    ~TContainer();

    std::string GetPortoNamespace() const;
    std::string ContainerStateName(EContainerState state);

    void AcquireForced();
    bool Acquire();
    void Release();
    bool IsAcquired() const;

    const std::string GetName() const;
    const std::string GetTextId(const std::string &separator = "+") const;
    const int GetId() const { return Id; }
    const int GetLevel() const { return Level; }
    uint64_t GetHierarchyMemGuarantee(void) const;

    bool IsRoot() const;
    bool IsPortoRoot() const;
    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<TContainer> GetParent() const;
    TError OpenNetns(TNamespaceFd &netns) const;

    std::vector<pid_t> Processes();

    TError SendSignal(int signal);
    TError SendTreeSignal(TScopedLock &holder_lock, int signal);
    void AddChild(std::shared_ptr<TContainer> child);
    TError Create(const TCred &cred);
    void Destroy(TScopedLock &holder_lock);
    void DestroyWeak();
    TError Start(std::shared_ptr<TClient> client, bool meta);
    TError Stop(TScopedLock &holder_lock);
    TError StopTree(TScopedLock &holder_lock);
    TError CheckPausedParent();
    TError CheckAcquiredChild(TScopedLock &holder_lock);
    TError Pause(TScopedLock &holder_lock);
    TError Resume(TScopedLock &holder_lock);
    TError Kill(int sig);

    TError GetProperty(const std::string &property, std::string &value,
                       std::shared_ptr<TClient> &client) const;
    TError SetProperty(const std::string &property, const std::string &value,
                       std::shared_ptr<TClient> &client);

    TError Restore(TScopedLock &holder_lock, const kv::TNode &node);
    TError Save(void);

    TCgroup GetCgroup(const TSubsystem &subsystem) const;
    bool CanRemoveDead() const;
    std::vector<std::string> GetChildren();
    std::shared_ptr<TContainer> FindRunningParent() const;
    void DeliverEvent(TScopedLock &holder_lock, const TEvent &event);

    TError CheckPermission(const TCred &ucred);

    static void ParsePropertyName(std::string &name, std::string &idx);
    size_t GetRunningChildren() { return RunningChildren; }

    void AddWaiter(std::shared_ptr<TContainerWaiter> waiter);

    bool IsLostAndRestored() const;
    void SyncStateWithCgroup(TScopedLock &holder_lock);
    void CleanupExpiredChildren();
    TError UpdateTrafficClasses();

    bool MayExit(int pid);
    bool MayRespawn();
    bool MayReceiveOom(int fd);
    bool HasOomReceived();

    bool IsFrozen();
    bool IsValid();

    std::shared_ptr<TVolumeHolder> VolumeHolder;

    /* protected with TVolumeHolder->Lock */
    std::vector<std::shared_ptr<TVolume>> Volumes;

    const TStdStream& GetStdin() const { return Stdin; }
    const TStdStream& GetStdout() const { return Stdout; }
    const TStdStream& GetStderr() const { return Stderr; }

    TError GetEnvironment(TEnv &env);
};

class TScopedAcquire : public TNonCopyable {
    std::shared_ptr<TContainer> Container;
    bool Acquired;

public:
    TScopedAcquire(std::shared_ptr<TContainer> c) : Container(c) {
        if (Container)
            Acquired = Container->Acquire();
        else
            Acquired = true;
    }
    ~TScopedAcquire() {
        if (Acquired && Container)
            Container->Release();
    }

    bool IsAcquired() { return Acquired; }
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
extern std::map<std::string, std::shared_ptr<TContainer>> Containers;

static inline std::unique_lock<std::mutex> LockContainers() {
    return std::unique_lock<std::mutex>(ContainersMutex);
}
