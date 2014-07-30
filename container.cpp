#include "container.hpp"
#include "task.hpp"

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

    try {
        string path = string("sleep");
        vector<string> args = { "10" };
        task = new TTask(path, args);
    } catch (const char *s) {
        cerr << "Start error: " << s << endl;
        // TODO: log
        return false;
    }

    state = Running;
    return true;
}

bool TContainer::Stop() {
    lock_guard<mutex> guard(lock);

    if (!CheckState(Running))
        return false;

    if (task->IsRunning())
        task->Kill();
    delete task;
    task = nullptr;

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

string TContainer::GetData(string data) {
    lock_guard<mutex> guard(lock);

    if (data == "root_pid") {
        if (!task)
            return "nil";
        return to_string(task->GetPid());
    } else if (data == "exit_status") {
        if (!task)
            return "nil";

        if (task->IsRunning())
            return "nil";

        return to_string(task->GetExitStatus());
    } else {
        return "nil";
    }
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
    containers.erase(name);
}

vector<string> TContainerHolder::List() {
    vector<string> ret;

    lock_guard<mutex> guard(lock);
    for (auto c : containers)
        ret.push_back(c.second->Name());

    return ret;
}
