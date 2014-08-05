#include <sstream>

#include "container.hpp"
#include "containerenv.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "registry.hpp"

TContainer::TContainer(const string name) : name(name), state(Stopped)
{
}

bool TContainer::CheckState(EContainerState expected) {
    if (state == Running && (!task || !task->IsRunning()))
        state = Stopped;

    return state == expected;
}

TContainer::~TContainer()
{
    Stop();

    state = Destroying;

    //TBD: perform actual work
}

string TContainer::Name()
{
    return name;
}

bool TContainer::Start()
{
    if (!CheckState(Stopped))
        return false;

    string command = GetProperty("command");
    string name = Name();

    vector<shared_ptr<TCgroup> > cgroups;

    // TODO: get real cgroups list

    auto mem = TRegistry<TSubsystem>::Get(TSubsystem("memory"));
    auto freezer = TRegistry<TSubsystem>::Get(TSubsystem("freezer"));
    set<shared_ptr<TSubsystem>> set;
    set.insert(mem);
    set.insert(freezer);
    auto rootmem = TRegistry<TCgroup>::Get(TCgroup(set));
    auto cg = TRegistry<TCgroup>::Get(TCgroup(name, rootmem));
    cgroups.push_back(cg);

    TTaskEnv taskEnv(command, "");

    auto *env = new TContainerEnv(cgroups, taskEnv);
    env->Create();

    task = new TTask(env);
    bool ret = task->Start();
    if (ret)
        state = Running;

    return ret;
}

bool TContainer::Stop()
{
    if (name == "/" || !CheckState(Running))
        return false;

    if (task->IsRunning())
        task->Kill();
    delete task;
    task = nullptr;

    // TODO: freeze and kill all other processes if any
    // Pause()
    // for each pid in cgroup:
    // auto task = TTask(pid)
    // task.kill()

    state = Stopped;
    return true;
}

bool TContainer::Pause()
{
    if (name == "/" || !CheckState(Running))
        return false;

    state = Paused;
    return true;
}

bool TContainer::Resume()
{
    if (!CheckState(Paused))
        return false;

    state = Running;
    return true;
}

string TContainer::GetData(string data)
{
    string tstat;

    if (data == "root_pid") {
        if (!task)
            return "0";
        return to_string(task->GetPid());
    } else if (data == "state") {
        switch (state) {
        case Stopped:
            return "stopped";
        case Running:
            if (!CheckState(Running))
                return "stopped";

            return "running";
        case Paused:
            return "paused";
        case Destroying:
            return "destroying";
        };
    } else if (data == "exit_status") {
        if (!task)
            return "nil";

        TExitStatus status = task->GetExitStatus();
        stringstream ss;
        //ss << status.error << ";" << status.signal << ";" << status.status;
        ss << "error=" << status.error << ";signal=" << status.signal << ";status=" << status.status;
        return ss.str();
    } else {
        return "nil";
    }
    return "unknown";
}

string TContainer::GetProperty(string property)
{
    auto val = properties.find(property);
    if (val == properties.end())
        return "";

    return val->second;
}

bool TContainer::SetProperty(string property, string value)
{
    if (name == "/")
        return false;
    if (property.length())
        properties[property] = value;
    else
        properties.erase(property);
    // TODO: write to persistent storage

    return true;
}

// TContainerHolder
TContainerHolder::TContainerHolder() {
    auto root = Create("/");
    root->Start();
}

TContainerHolder::~TContainerHolder() {
    Destroy("/");
}

shared_ptr<TContainer> TContainerHolder::Create(string name)
{
    if (containers[name] == nullptr)
        containers[name] = make_shared<TContainer>(name);
    else
        throw "container " + name + " already exists";

    return containers[name];
}

shared_ptr<TContainer> TContainerHolder::Find(string name)
{
    return containers[name];
}

void TContainerHolder::Destroy(string name)
{
    containers[name] = nullptr;
}

vector<string> TContainerHolder::List()
{
    vector<string> ret;

    for (auto c : containers)
        ret.push_back(c.second->Name());

    return ret;
}
