#include <sys/types.h>
#include <unistd.h>

#include <sstream>
#include <memory>

#include "container.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "registry.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
}

using namespace std;

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
    if (state == Paused)
        Resume();

    Stop();
}

string TContainer::Name()
{
    return name;
}

bool TContainer::IsRoot() {
    return name == "/";
}

vector<pid_t> TContainer::Processes() {
    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    return cg->Processes();
}

bool TContainer::IsAlive() {
    return IsRoot() || !Processes().empty();
}

bool TContainer::Start()
{
    if (!CheckState(Stopped))
        return false;

    if (IsRoot()) {
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Memory()));
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Freezer()));
    } else {
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Memory())));
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer())));
    }

    for (auto cg : leaf_cgroups)
        cg->Create();

    if (IsRoot())
        return true;

    TTaskEnv taskEnv(GetProperty("command"), "");
    task = unique_ptr<TTask>(new TTask(taskEnv, [&this->leaf_cgroups] () {
            pid_t self = getpid();

            for (auto cg : leaf_cgroups) {
                auto error = cg->Attach(self);
                if (error)
                    return error;
            }

            return TError();
        }));

    bool ret = task->Start();
    if (ret)
        state = Running;

    return ret;
}

bool TContainer::Stop()
{
    if (name == "/" || !CheckState(Running))
        return false;

    Pause();
    Kill(SIGTERM);
    Resume();
    usleep(100000);
    
    while (IsAlive()) {
        Pause();
        Kill(SIGKILL);
        Resume();
    }

    task = nullptr;

    state = Stopped;

    return true;
}

bool TContainer::Kill(int signal)
{
    if (name == "/")
        return false;

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    for (auto t : cg->Tasks()) {
        TTask task(t);
        task.Kill(signal);
    }

    return true;
}

bool TContainer::Pause()
{
    if (name == "/" || !CheckState(Running))
        return false;

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Freeze(*cg);

    state = Paused;
    return true;
}

bool TContainer::Resume()
{
    if (!CheckState(Paused))
        return false;

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Unfreeze(*cg);

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
    if (name != "/")
        containers.erase(name);
}

vector<string> TContainerHolder::List()
{
    vector<string> ret;

    for (auto c : containers)
        ret.push_back(c.second->Name());

    return ret;
}
