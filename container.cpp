#include <sstream>

#include "container.hpp"
#include "containerenv.hpp"
#include "task.hpp"

#include "cgroup.hpp"

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

    {
        lock_guard<mutex> guard(lock);
        state = Destroying;
    }

    //TBD: perform actual work
}

string TContainer::Name()
{
    return name;
}

bool TContainer::Start()
{
    lock_guard<mutex> guard(lock);

    if (!CheckState(Stopped))
        return false;

    string command = GetProperty("command");
    string name = Name();

    vector<shared_ptr<TCgroup> > cgroups;

    // TODO: get real cgroups list
    //TCgroupSnapshot cs;
#if 0
    if (false) {
        auto mem = new TSubsys("memory");
        set<TSubsys *> cset;
        cset.insert(mem);
        auto rootmem = new TRootCgroup(cset);
        auto cg = new TCgroup(name, rootmem, rootmem);
        cgroups.push_back(cg);
    }
#endif

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
    lock_guard<mutex> guard(lock);

    if (!CheckState(Running))
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
    lock_guard<mutex> guard(lock);

    if (!CheckState(Running))
        return false;

    state = Paused;
    return true;
}

bool TContainer::Resume()
{
    lock_guard<mutex> guard(lock);

    if (!CheckState(Paused))
        return false;

    state = Running;
    return true;
}

string TContainer::GetData(string data)
{
    lock_guard<mutex> guard(lock);
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

string TContainer::GetPropertyLocked(string property)
{
    auto val = properties.find(property);
    if (val == properties.end())
        return "";

    return val->second;
}

string TContainer::GetProperty(string property)
{
    lock_guard<mutex> guard(lock);

    return GetPropertyLocked(property);
}

bool TContainer::SetProperty(string property, string value)
{
    lock_guard<mutex> guard(lock);

    if (property.length())
        properties[property] = value;
    else
        properties.erase(property);
    // TODO: write to persistent storage

    return true;
}

TContainer* TContainerHolder::Create(string name)
{
    lock_guard<mutex> guard(lock);

    if (containers[name] == nullptr)
        containers[name] = new TContainer(name);
    else
        throw "container " + name + " already exists";

    return containers[name];
}

TContainer* TContainerHolder::Find(string name)
{
    lock_guard<mutex> guard(lock);

    return containers[name];
}

void TContainerHolder::Destroy(string name)
{
    lock_guard<mutex> guard(lock);
    delete containers[name];
    containers.erase(name);
}

vector<string> TContainerHolder::List()
{
    vector<string> ret;

    lock_guard<mutex> guard(lock);
    for (auto c : containers)
        ret.push_back(c.second->Name());

    return ret;
}
