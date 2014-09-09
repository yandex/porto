#ifndef __CONTAINER_H__
#define __CONTAINER_H__

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <set>

#include "kvalue.hpp"
#include "property.hpp"
#include "task.hpp"

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
    std::vector<std::weak_ptr<TContainer>> Children;
    EContainerState State;
    TContainerSpec Spec;
    bool MaybeReturnedOk = false;
    size_t TimeOfDeath = 0;
    friend TData;

    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;
    std::unique_ptr<TTask> Task;

    // data
    bool CheckState(EContainerState expected);
    TError PrepareCgroups();
    TError PrepareTask();
    TError KillAll();

    TContainer(const TContainer &) = delete;
    TContainer &operator=(const TContainer &) = delete;
    const std::string StripParentName(const std::string &name) const;
    bool NeedRespawn();
    TError Respawn();

public:
    TContainer(const std::string &name, std::shared_ptr<TContainer> parent) :
        Name(StripParentName(name)), Parent(parent), State(EContainerState::Stopped), Spec(name) { }
    ~TContainer();

    const std::string GetName() const;

    bool IsRoot() const;
    std::shared_ptr<const TContainer> GetRoot() const;
    std::shared_ptr<const TContainer> GetParent() const;

    uint64_t GetMemGuarantee() const;
    uint64_t GetChildrenMemGuarantee(std::shared_ptr<const TContainer> except = nullptr, uint64_t exceptVal = 0) const;
    std::vector<pid_t> Processes();
    bool IsAlive();

    TError Create();
    TError Start();
    TError Stop();
    TError Pause();
    TError Resume();

    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value);

    TError GetData(const std::string &data, std::string &value);
    TError Restore(const kv::TNode &node);

    bool DeliverExitStatus(int pid, int status);

    std::shared_ptr<TCgroup> GetLeafCgroup(std::shared_ptr<TSubsystem> subsys);
    void Heartbeat();
    bool CanRemoveDead() const;
    bool HasChildren() const;
};

class TContainerHolder {
    std::map <std::string, std::shared_ptr<TContainer>> Containers;

    bool ValidName(const std::string &name) const;
public:
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
};

#endif
