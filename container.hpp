#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <set>

#include "kvalue.hpp"
#include "task.hpp"
#include "util/unix.hpp"

class TEpollSource;
class TCgroup;
class TSubsystem;
class TPropertyMap;
class TValueMap;
enum class ETclassStat;
class TEvent;
class TContainerHolder;
class TNetwork;
class TTclass;
class TTask;
class TContainerWaiter;
class TClient;
class TVolume;

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
                   public TNonCopyable {
    std::shared_ptr<TContainerHolder> Holder;
    const std::string Name;
    const std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TTclass> Tclass;
    std::vector<std::weak_ptr<TContainer>> Children;
    std::shared_ptr<TKeyValueStorage> Storage;
    EContainerState State = EContainerState::Unknown;
    uint16_t Id;
    int TaskStartErrno = -1;
    TScopedFd Efd;
    size_t CgroupEmptySince = 0;
    size_t RunningChildren = 0;
    bool LostAndRestored = false;
    std::unique_ptr<TTask> Task;
    std::vector<std::weak_ptr<TContainerWaiter>> Waiters;

    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;
    std::shared_ptr<TEpollSource> Source;

    // data
    void UpdateRunningChildren(size_t diff);
    TError UpdateSoftLimit();
    void SetState(EContainerState newState, bool tree = false);
    std::string ContainerStateName(EContainerState state);

    TError ApplyDynamicProperties();
    TError PrepareNetwork();
    TError PrepareOomMonitor();
    void ShutdownOom();
    TError PrepareCgroups();
    TError PrepareTask();
    TError KillAll();
    void RemoveKvs();

    const std::string StripParentName(const std::string &name) const;
    void ScheduleRespawn();
    bool MayRespawn();
    bool ShouldApplyProperty(const std::string &property);
    TError Respawn();
    bool StopChildren();
    TError PrepareResources();
    void FreeResources();
    void PropertyToAlias(const std::string &property, std::string &value) const;
    TError AliasToProperty(std::string &property, std::string &value);

    bool Exit(int status, bool oomKilled, bool force = false);
    bool ExitChildren(int status, bool oomKilled);
    bool DeliverExitStatus(int pid, int status);
    bool DeliverOom(int fd);

    TError Prepare();

    std::string GetPortoNamespace() const;

    void CleanupWaiters();
    void NotifyWaiters();

public:
    TCred OwnerCred;

    // TODO: make private
    std::shared_ptr<TPropertyMap> Prop;
    std::shared_ptr<TValueMap> Data;
    std::shared_ptr<TNetwork> Net;

    std::string GetTmpDir() const;
    EContainerState GetState() const;
    TError GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m);

    TContainer(std::shared_ptr<TContainerHolder> holder,
               std::shared_ptr<TKeyValueStorage> storage,
               const std::string &name, std::shared_ptr<TContainer> parent,
               uint16_t id, std::shared_ptr<TNetwork> net) :
        Holder(holder), Name(StripParentName(name)), Parent(parent),
        Storage(storage), Id(id), Net(net) { }

    const std::string GetName(bool recursive = true, const std::string &sep = "/") const;
    const uint16_t GetId() const { return Id; }

    bool IsRoot() const;
    bool IsPortoRoot() const;
    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<const TContainer> GetParent() const;
    bool ValidLink(const std::string &name) const;
    std::shared_ptr<TNlLink> GetLink(const std::string &name) const;

    uint64_t GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except = nullptr, uint64_t exceptVal = 0) const;
    bool ValidHierarchicalProperty(const std::string &property, const uint64_t value) const;
    std::vector<pid_t> Processes();

    TError Create(const TCred &cred);
    TError Destroy();
    TError Start(bool meta);
    TError Stop();
    TError Pause();
    TError Resume();
    TError Kill(int sig);

    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value, bool superuser);

    TError GetData(const std::string &data, std::string &value);
    TError Restore(const kv::TNode &node);

    std::shared_ptr<TCgroup> GetLeafCgroup(std::shared_ptr<TSubsystem> subsys);
    bool CanRemoveDead() const;
    std::vector<std::string> GetChildren();
    std::shared_ptr<TContainer> FindRunningParent() const;
    bool UseParentNamespace() const;
    bool DeliverEvent(const TEvent &event);

    TError CheckPermission(const TCred &ucred);

    // *self is observer container
    TError RelativeName(const TContainer &c, std::string &name) const;
    TError AbsoluteName(const std::string &orig, std::string &name,
                        bool resolve_meta = false) const;

    static void ParsePropertyName(std::string &name, std::string &idx);
    size_t GetRunningChildren() { return RunningChildren; }

    void AddWaiter(std::shared_ptr<TContainerWaiter> waiter);

    bool IsLostAndRestored() const;
    void SyncStateWithCgroup();
    bool IsNamespaceIsolated();

    /* protected with TVolumeHolder->Lock */
    std::set<std::shared_ptr<TVolume>> Volumes;

    bool LinkVolume(std::shared_ptr<TVolume> volume) {
        return Volumes.insert(volume).second;
    }

    bool UnlinkVolume(std::shared_ptr<TVolume> volume) {
        return Volumes.erase(volume);
    }
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
