#pragma once

#include <string>
#include <vector>
#include <memory>
#include <set>

#include "util/unix.hpp"
#include "util/locks.hpp"
#include "util/log.hpp"

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
class TNlLink;
class TTclass;
class TTask;
class TContainerWaiter;
class TClient;
class TVolume;
class TVolumeHolder;

namespace kv {
    class TNode;
};

extern int64_t BootTime;

enum class EContainerState {
    Unknown,
    Stopped,
    Dead,
    Running,
    Paused,
    Meta
};

class TContainer : public std::enable_shared_from_this<TContainer>,
                   public TNonCopyable,
                   public TLockable {
    std::shared_ptr<TContainerHolder> Holder;
    const std::string Name;
    const std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TTclass> Tclass;
    std::vector<std::weak_ptr<TContainer>> Children;
    std::shared_ptr<TKeyValueStorage> Storage;
    EContainerState State = EContainerState::Unknown;
    int Acquired = 0;
    uint16_t Id;
    int TaskStartErrno = -1;
    TScopedFd Efd;
    size_t CgroupEmptySince = 0;
    size_t RunningChildren = 0; // changed under holder lock
    bool LostAndRestored = false;
    std::unique_ptr<TTask> Task;
    std::vector<std::weak_ptr<TContainerWaiter>> Waiters;

    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;
    std::shared_ptr<TEpollSource> Source;
    bool IsMeta = false;

    std::ofstream JournalStream;

    // data
    void UpdateRunningChildren(size_t diff);
    TError UpdateSoftLimit();
    void SetState(EContainerState newState);
    std::string ContainerStateName(EContainerState state);

    TError ApplyDynamicProperties();
    TError PrepareNetwork();
    TError PrepareOomMonitor();
    TError PrepareLoop();
    void ShutdownOom();
    TError PrepareCgroups();
    TError PrepareTask(std::shared_ptr<TClient> client);
    TError KillAll(TScopedLock &holder_lock);
    void RemoveKvs();

    const std::string StripParentName(const std::string &name) const;
    void ScheduleRespawn();
    bool ShouldApplyProperty(const std::string &property);
    TError Respawn(TScopedLock &holder_lock);
    void StopChildren(TScopedLock &holder_lock);
    TError PrepareResources();
    void RemoveLog(const TPath &path);
    TError RotateLog(const TPath &path);
    void FreeResources();
    void PropertyToAlias(const std::string &property, std::string &value) const;
    TError AliasToProperty(std::string &property, std::string &value);

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

    bool PrepareJournal();

public:
    TCred OwnerCred;

    // TODO: make private
    std::shared_ptr<TPropertyMap> Prop;
    std::shared_ptr<TValueMap> Data;
    std::shared_ptr<TNetwork> Net;

    TPath GetTmpDir() const;
    TPath RootPath() const;
    TPath DefaultStdFile(const std::string prefix) const;
    EContainerState GetState() const;
    TError GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m);

    TContainer(std::shared_ptr<TContainerHolder> holder,
               std::shared_ptr<TKeyValueStorage> storage,
               const std::string &name, std::shared_ptr<TContainer> parent,
               uint16_t id, std::shared_ptr<TNetwork> net);
    ~TContainer();

    std::string GetPortoNamespace() const;

    void AcquireForced();
    bool Acquire();
    void Release();
    bool IsAcquired() const;

    const std::string GetName(bool recursive = true, const std::string &sep = "/") const;
    const uint16_t GetId() const { return Id; }

    bool IsRoot() const;
    bool IsPortoRoot() const;
    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<TContainer> GetParent() const;
    bool ValidLink(const std::string &name) const;
    std::shared_ptr<TNlLink> GetLink(const std::string &name) const;

    template <typename T>
    T GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except = nullptr, T exceptVal = 0) const;
    template <typename T>
    bool ValidHierarchicalProperty(const std::string &property, const T value) const;

    std::vector<pid_t> Processes();

    TError SendSignal(int signal);
    TError SendTreeSignal(TScopedLock &holder_lock, int signal);
    void AddChild(std::shared_ptr<TContainer> child);
    TError Create(const TCred &cred);
    void Destroy(TScopedLock &holder_lock);
    TError Start(std::shared_ptr<TClient> client, bool meta);
    TError Stop(TScopedLock &holder_lock);
    TError StopTree(TScopedLock &holder_lock);
    TError CheckPausedParent();
    TError CheckAcquiredChild(TScopedLock &holder_lock);
    TError Pause(TScopedLock &holder_lock);
    TError Resume(TScopedLock &holder_lock);
    TError Kill(int sig);

    TError GetProperty(const std::string &property, std::string &value,
                       std::shared_ptr<TClient> client) const;
    TError SetProperty(const std::string &property,
                       const std::string &value, std::shared_ptr<TClient> client);

    TError GetData(const std::string &data, std::string &value,
                   std::shared_ptr<TClient> client);
    TError Restore(TScopedLock &holder_lock, const kv::TNode &node);

    std::shared_ptr<TCgroup> GetLeafCgroup(std::shared_ptr<TSubsystem> subsys);
    bool CanRemoveDead() const;
    std::vector<std::string> GetChildren();
    std::shared_ptr<TContainer> FindRunningParent() const;
    bool UseParentNamespace() const;
    void DeliverEvent(TScopedLock &holder_lock, const TEvent &event);

    TError CheckPermission(const TCred &ucred);

    // *self is observer container
    TError RelativeName(const TContainer &c, std::string &name) const;
    TError AbsoluteName(const std::string &orig, std::string &name,
                        bool resolve_meta = false) const;

    static void ParsePropertyName(std::string &name, std::string &idx);
    size_t GetRunningChildren() { return RunningChildren; }

    void AddWaiter(std::shared_ptr<TContainerWaiter> waiter);

    bool IsLostAndRestored() const;
    void SyncStateWithCgroup(TScopedLock &holder_lock);
    bool IsNamespaceIsolated();
    void CleanupExpiredChildren();
    TError UpdateNetwork();

    bool MayExit(int pid);
    bool MayRespawn();
    bool MayReceiveOom(int fd);

    bool IsFrozen();
    bool IsValid();

    std::shared_ptr<TVolumeHolder> VolumeHolder;
    /* protected with TVolumeHolder->Lock */
    std::set<std::shared_ptr<TVolume>> Volumes;

    bool LinkVolume(std::shared_ptr<TVolumeHolder> holder,
                    std::shared_ptr<TVolume> volume) {
        if (!VolumeHolder)
            VolumeHolder = holder;
        else
            PORTO_ASSERT(VolumeHolder == holder);
        return Volumes.insert(volume).second;
    }

    bool UnlinkVolume(std::shared_ptr<TVolume> volume) {
        return Volumes.erase(volume);
    }

    // raw
    void Journal(const std::string &message);
    // for user's actions
    void Journal(const std::string &message, std::shared_ptr<TClient> client);
    // for recursive actions, like stopping tree of containers
    void Journal(const std::string &message, std::shared_ptr<TContainer> root);
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
    std::weak_ptr<TClient> Client;
    std::function<void (std::shared_ptr<TClient>, TError, std::string)> Callback;
public:
    TContainerWaiter(std::shared_ptr<TClient> client,
                     std::function<void (std::shared_ptr<TClient>, TError, std::string)> callback);
    void Signal(const TContainer *who);
};
