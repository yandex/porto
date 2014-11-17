#ifndef __CONTAINER_H__
#define __CONTAINER_H__

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <set>
#include <climits>

#include "kvalue.hpp"
#include "task.hpp"
#include "qdisc.hpp"
#include "util/unix.hpp"

class TCgroup;
class TContainerEnv;
struct TData;
class TContainer;
class TSubsystem;
class TPropertySet;
class TVariantSet;
class TValueSet;
enum class ETclassStat;
class TEventQueue;
class TEvent;

extern int64_t BootTime;

enum class EContainerState {
    Unknown,
    Stopped,
    Dead,
    Running,
    Paused,
    Meta
};

struct TDataSpec {
    std::string Description;
    unsigned int Flags;
    std::function<std::string(TContainer& c)> Handler;
    std::set<EContainerState> Valid;
};

class TContainer : public std::enable_shared_from_this<TContainer> {
    NO_COPY_CONSTRUCT(TContainer);
    std::shared_ptr<TEventQueue> Queue;
    const std::string Name;
    const std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TQdisc> Qdisc;
    std::shared_ptr<TTclass> Tclass, DefaultTclass;
    std::shared_ptr<TFilter> Filter;
    std::vector<std::weak_ptr<TContainer>> Children;
    std::shared_ptr<TKeyValueStorage> Storage;
    EContainerState State = EContainerState::Unknown;
    size_t TimeOfDeath = 0;
    uint16_t Id;
    int TaskStartErrno = -1;
    TScopedFd Efd;

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

    bool DeliverExitStatus(int pid, int status);

    void ParseName(std::string &name, std::string &idx) const;
    TError Prepare();

public:
    int Uid, Gid;

    // TODO: make private
    std::unique_ptr<TTask> Task;
    std::shared_ptr<TPropertySet> Prop;
    std::shared_ptr<TVariantSet> Data;
    std::vector<std::shared_ptr<TNlLink>> Links;

    std::string GetTmpDir() const;
    EContainerState GetState();
    TError GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m) { return Tclass->GetStat(stat, m); }

    TContainer(std::shared_ptr<TEventQueue> queue,
               const std::string &name, std::shared_ptr<TContainer> parent,
               uint16_t id, const std::vector<std::shared_ptr<TNlLink>> &links) :
        Queue(queue), Name(StripParentName(name)), Parent(parent), Id(id),
        Links(links) { }
    ~TContainer();

    const std::string GetName(bool recursive = true) const;
    const uint16_t GetId() const { return Id; }

    bool IsRoot() const;
    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<const TContainer> GetParent() const;
    bool ValidLink(const std::string &name) const;
    std::shared_ptr<TNlLink> GetLink(const std::string &name) const;

    uint64_t GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except = nullptr, uint64_t exceptVal = 0) const;
    bool ValidHierarchicalProperty(const std::string &property, const uint64_t value) const;
    std::vector<pid_t> Processes();

    TError Create(int uid, int gid);
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
    bool HasChildren() const;
    uint16_t GetId() { return Id; }
    std::shared_ptr<TContainer> FindRunningParent() const;
    bool UseParentNamespace() const;
    bool DeliverEvent(const TEvent &event);
};

constexpr size_t BITS_PER_LLONG = sizeof(unsigned long long) * 8;
class TContainerHolder {
    NO_COPY_CONSTRUCT(TContainerHolder);
    std::shared_ptr<TEventQueue> Queue;
    std::vector<std::shared_ptr<TNlLink>> Links;
    std::map <std::string, std::shared_ptr<TContainer>> Containers;
    unsigned long long Ids[UINT16_MAX / BITS_PER_LLONG];

    bool ValidName(const std::string &name) const;
    TError GetId(uint16_t &id);
    void PutId(uint16_t id);
    TError RestoreId(const kv::TNode &node, uint16_t &id);
    void ScheduleLogRotatation();
public:
    TContainerHolder(std::shared_ptr<TEventQueue> queue,
                     const std::vector<std::shared_ptr<TNlLink>> &links) :
        Queue(queue), Links(links) {
        for (auto &i : Ids) { i = ULLONG_MAX; }
    }
    ~TContainerHolder();
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot();
    TError Create(const std::string &name, int uid, int gid);
    std::shared_ptr<TContainer> Get(const std::string &name);
    TError Restore(const std::string &name, const kv::TNode &node);
    TError CheckPermission(std::shared_ptr<TContainer> container, int uid, int gid);
    TError Destroy(const std::string &name);

    std::vector<std::string> List() const;

    bool DeliverEvent(const TEvent &event);
};

#endif
