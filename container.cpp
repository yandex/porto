#include "container.h"

TContainer::TContainer(const string _name) : name(_name), state(Stopped) {
}

TContainer::~TContainer() {
    lock_guard<mutex> guard(lock);
    state = Destroying;

    //TBD: perform actual work
}

string TContainer::Name() {
    return name;
}

bool TContainer::Start() {
    lock_guard<mutex> guard(lock);

    if (!CheckState(Stopped))
        return false;

    state = Running;
    return true;
}

bool TContainer::Stop() {
    lock_guard<mutex> guard(lock);

    if (!CheckState(Running))
        return false;

    state = Stopped;
    return true;
}

bool TContainer::Pause() {
    lock_guard<mutex> guard(lock);

    if (!CheckState(Running))
        return false;

    state = Paused;
    return true;
}

bool TContainer::Resume() {
    lock_guard<mutex> guard(lock);

    if (!CheckState(Paused))
        return false;

    state = Running;
    return true;
}

TContainer* TContainerHolder::Create(string name) {
    lock_guard<mutex> guard(lock);

    if (containers[name] == nullptr)
        containers[name] = new TContainer(name);
    else
        throw "container " + name + " already exists";

    return containers[name];
}

TContainer* TContainerHolder::Find(string name) {
    lock_guard<mutex> guard(lock);

    return containers[name];
}

void TContainerHolder::Destroy(string name) {
    lock_guard<mutex> guard(lock);
    delete containers[name];
    containers[name] = nullptr;
}

vector<string> TContainerHolder::List() {
    vector<string> ret;

    lock_guard<mutex> guard(lock);
    for (auto c : containers)
        ret.push_back(c.second->Name());

    return ret;
}
