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
#include "property.hpp"
#include "task.hpp"
#include "qdisc.hpp"
#include "util/unix.hpp"

class TCgroup;
class TContainerEnv;
struct TData;
class TContainer;
class TSubsystem;

enum class EContainerState {
    Stopped,
    Dead,
    Running,
    Paused
};

struct TDataSpec {
    std::string Description;
    bool RootValid;
    std::function<std::string(TContainer& c)> Handler;
    std::set<EContainerState> Valid;
};

extern std::map<std::string, const TDataSpec> dataSpec;

class TContainer : public std::enable_shared_from_this<TContainer> {
    const std::string Name;
    const std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TQdisc> Qdisc;
    std::shared_ptr<TTclass> Tclass, DefaultTclass;
    std::shared_ptr<TFilter> Filter;
    std::vector<std::weak_ptr<TContainer>> Children;
    EContainerState State;
    TContainerSpec Spec;
    bool MaybeReturnedOk = false;
    size_t TimeOfDeath = 0;
    uint16_t Id;
    int TaskStartErrno = -1;
    TScopedFd Efd;
    bool OomKilled = false;
    friend TData;

    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;
    std::unique_ptr<TTask> Task;

    // data
    bool CheckState(EContainerState expected);
    TError ApplyDynamicProperties();
    TError PrepareNetwork();
    TError PrepareOomMonitor();
    TError PrepareCgroups();
    TError PrepareTask();
    TError KillAll();

    NO_COPY_CONSTRUCT(TContainer);
    const std::string StripParentName(const std::string &name) const;
    bool NeedRespawn();
    bool ShouldApplyProperty(const std::string &property);
    TError Respawn();
    uint64_t GetPropertyUint64(const std::string &property) const;
    void StopChildren();
    void FreeResources();

public:
    TContainer(const std::string &name, std::shared_ptr<TContainer> parent, uint16_t id) :
        Name(StripParentName(name)), Parent(parent), State(EContainerState::Stopped), Spec(name), Id(id) { }
    ~TContainer();

    const std::string GetName() const;

    bool IsRoot() const;
    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<const TContainer> GetParent() const;

    uint64_t GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except = nullptr, uint64_t exceptVal = 0) const;
    bool ValidHierarchicalProperty(const std::string &property, const std::string &value) const;
    std::vector<pid_t> Processes();
    bool IsAlive();

    TError Create();
    TError Start();
    TError Stop();
    TError Pause();
    TError Resume();
    TError Kill(int sig);

    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value);

    TError GetData(const std::string &data, std::string &value);
    TError Restore(const kv::TNode &node);

    bool DeliverExitStatus(int pid, int status);

    std::shared_ptr<TCgroup> GetLeafCgroup(std::shared_ptr<TSubsystem> subsys);
    void Heartbeat();
    bool CanRemoveDead() const;
    bool HasChildren() const;
    uint16_t GetId();
    int GetOomFd();
    void DeliverOom();
};

constexpr size_t BITS_PER_LLONG = sizeof(unsigned long long) * 8;
class TContainerHolder {
    std::map <std::string, std::shared_ptr<TContainer>> Containers;
    unsigned long long Ids[UINT16_MAX / BITS_PER_LLONG];

    bool ValidName(const std::string &name) const;
    TError GetId(uint16_t &id);
    void PutId(uint16_t id);
    TError RestoreId(const kv::TNode &node, uint16_t &id);
public:
    TContainerHolder() { for (auto &i : Ids) { i = ULLONG_MAX; } }
    ~TContainerHolder();
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot();
    TError Create(const std::string &name);
    std::shared_ptr<TContainer> Get(const std::string &name);
    TError Restore(const std::string &name, const kv::TNode &node);

    TError Destroy(const std::string &name);
    bool DeliverExitStatus(int pid, int status);

    std::vector<std::string> List() const;
    void Heartbeat();
    void PushOomFds(std::vector<int> &fds);
    void DeliverOom(int fd);
};

#endif
