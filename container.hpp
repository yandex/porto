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

class TTask;
class TCgroup;
class TContainerEnv;
class TData;

class TContainer {
    const std::string name;

    enum EContainerState {
        Stopped,
        Running,
        Paused
    };
    EContainerState state;

    TContainerSpec spec;
    friend TData;
    std::map<std::string, std::function<std::string(TContainer& c)>> data;

    std::vector<std::shared_ptr<TCgroup> > leaf_cgroups;
    std::unique_ptr<TTask> task;

    // data
    bool CheckState(EContainerState expected);

public:
    TContainer(const std::string name);
    ~TContainer();

    string Name();

    bool IsRoot();
    
    vector<pid_t> Processes();
    bool IsAlive();

    bool Kill(int SIGNAL);

    bool Start();
    bool Stop();
    bool Pause();
    bool Resume();

    std::string GetProperty(std::string property);
    bool SetProperty(std::string property, std::string value);

    string GetData(std::string data);
};

class TContainerHolder {
    std::unordered_map <std::string, std::shared_ptr<TContainer> > containers;

public:
    TContainerHolder();
    ~TContainerHolder();

    std::shared_ptr<TContainer> Create(std::string name);
    std::shared_ptr<TContainer> Find(std::string name);

    void Destroy(std::string name);

    vector<std::string> List();
};

#endif
