#ifndef __CONTAINER_H__
#define __CONTAINER_H__

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

#include "kvalue.hpp"
#include "property.hpp"
#include "task.hpp"

class TCgroup;
class TContainerEnv;
struct TData;
class TContainer;
class TSubsystem;

struct TDataSpec {
    std::string description;
    std::function<std::string(TContainer& c)> Handler;
};

extern std::map<std::string, const TDataSpec> dataSpec;

class TContainer {
    const std::string name;

    enum EContainerState {
        Stopped,
        Dead,
        Running,
        Paused
    };
    EContainerState state;

    TContainerSpec spec;
    friend TData;

    std::vector<std::shared_ptr<TCgroup>> leaf_cgroups;
    std::unique_ptr<TTask> task;

    // data
    bool CheckState(EContainerState expected);
    TError PrepareCgroups();
    TError PrepareTask();
    TError KillAll();

public:
    TContainer(const std::string &name) : name(name), state(Stopped), spec(name) { }
    ~TContainer();

    std::string Name();

    bool IsRoot();

    std::vector<pid_t> Processes();
    bool IsAlive();

    void UpdateState();

    TError Start();
    TError Stop();
    TError Pause();
    TError Resume();

    TError GetProperty(const std::string &property, std::string &value);
    TError SetProperty(const std::string &property, const std::string &value);

    TError GetData(const std::string &data, std::string &value);
    TError Restore(const kv::TNode &node);

    bool DeliverExitStatus(int pid, int status);

    std::shared_ptr<TCgroup> GetCgroup(std::shared_ptr<TSubsystem> subsys);
    void Heartbeat();
};

class TContainerHolder {
    std::map <std::string, std::shared_ptr<TContainer>> containers;

    bool ValidName(const std::string &name);
public:
    TError CreateRoot();
    TError Create(const std::string &name);
    std::shared_ptr<TContainer> Get(const std::string &name);
    TError Restore(const std::string &name, const kv::TNode &node);

    void Destroy(const std::string &name);
    bool DeliverExitStatus(int pid, int status);

    std::vector<std::string> List();
    void Heartbeat();
};

#endif
