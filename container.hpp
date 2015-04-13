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
    size_t TimeOfDeath = 0;
    uint16_t Id;
    int TaskStartErrno = -1;
    TScopedFd Efd;
    size_t CgroupEmptySince = 0;

    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;

    // data
    bool HaveRunningChildren();
    void SetState(EContainerState newState);
    std::string ContainerStateName(EContainerState state);

    TError ApplyDynamicProperties();
    TError PrepareNetwork();
    TError PrepareOomMonitor();
    TError PrepareCgroups();
    TError PrepareTask();
    TError KillAll();
    TError PrepareMetaParent();

    const std::string StripParentName(const std::string &name) const;
    void ScheduleRespawn();
    bool MayRespawn();
    bool ShouldApplyProperty(const std::string &property);
    TError Respawn();
    void StopChildren();
    TError PrepareResources();
    void FreeResources();
    void PropertyToAlias(const std::string &property, std::string &value) const;
    TError AliasToProperty(std::string &property, std::string &value);

    bool Exit(int status, bool oomKilled);
    bool DeliverExitStatus(int pid, int status);
    bool DeliverOom(int fd);

    void ParseName(std::string &name, std::string &idx) const;
    TError Prepare();

public:
    TCred OwnerCred;

    // TODO: make private
    std::unique_ptr<TTask> Task;
    std::shared_ptr<TPropertyMap> Prop;
    std::shared_ptr<TValueMap> Data;
    std::shared_ptr<TNetwork> Net;

    std::string GetTmpDir() const;
    EContainerState GetState();
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
    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<const TContainer> GetParent() const;
    bool ValidLink(const std::string &name) const;
    std::shared_ptr<TNlLink> GetLink(const std::string &name) const;

    uint64_t GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except = nullptr, uint64_t exceptVal = 0) const;
    bool ValidHierarchicalProperty(const std::string &property, const uint64_t value) const;
    std::vector<pid_t> Processes();

    TError Create(const TCred &cred);
    TError Destroy();
    TError Start();
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
    size_t GetTimeOfDeath() { return TimeOfDeath; }

    TError CheckPermission(const TCred &ucred);
};
