#ifndef __CONTAINER_H__
#define __CONTAINER_H__

#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <memory>

#include "kvalue.hpp"

class TTask;

class TContainer {
    const std::string name;

    enum EContainerState {
        Stopped,
        Running,
        Paused,
        Destroying
    };
    EContainerState state;
    TTask *task;
    std::map<std::string, std::string> properties;

    // data
    bool CheckState(EContainerState expected);
    string GetPropertyLocked(string property);

public:
    TContainer(const std::string name);
    ~TContainer();

    string Name();

    bool Start();
    bool Stop();
    bool Pause();
    bool Resume();

    std::string GetProperty(std::string property);
    bool SetProperty(std::string property, std::string value);

    string GetData(std::string data);
};

class TContainerHolder {
    std::map <std::string, std::shared_ptr<TContainer> > containers;

public:
    std::shared_ptr<TContainer> Create(std::string name);
    std::shared_ptr<TContainer> Find(std::string name);

    void Destroy(std::string name);

    vector<std::string> List();
};

#endif
