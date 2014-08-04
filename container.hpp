#ifndef __CONTAINER_H__
#define __CONTAINER_H__

#include <iostream>
#include <string>
#include <mutex>
#include <map>
#include <vector>

#include "kvalue.hpp"

class TTask;

class TContainer {
    const std::string name;

    std::mutex lock;
    enum EContainerState {
        Stopped,
        Running,
        Paused,
        Destroying
    };
    EContainerState state;
    TTask *task;
    std::map<std::string, std::string> properties;

    std::mutex _data_lock;
    // data

    bool CheckState(EContainerState expected);
    string GetPropertyLocked(string property);

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
    std::mutex lock;
    std::map <std::string, TContainer*> containers;

    public:
    TContainer* Create(std::string name);
    TContainer* Find(std::string name);

    void Destroy(std::string name);

    vector<std::string> List();
};

#endif
