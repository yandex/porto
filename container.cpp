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

// Data

struct TData {
    static string State(TContainer& c) {
        c.UpdateState();

        switch (c.state) {
        case TContainer::Stopped:
            return "stopped";
        case TContainer::Running:
            return "running";
        case TContainer::Paused:
            return "paused";
        default:
            return "unknown";
        }
    }

    static string RootPid(TContainer& c) {
        if (c.task)
            return to_string(c.task->GetPid());
        else
            return "unknown";
    };

    static string ExitStatus(TContainer& c) {
        if (c.task) {
            TExitStatus status = c.task->GetExitStatus();
            stringstream ss;
            //ss << status.error << ";" << status.signal << ";" << status.status;
            ss << "error=" << status.error << ";signal=" << status.signal << ";status=" << status.status;
            return ss.str();
        }
        else
            return "unknown";
    };
};

std::map<std::string, const TDataSpec> dataSpec = {
    { "state", { "container state", TData::State } },
    { "root_pid", { "root process id", TData::RootPid } },
};

// TContainer

TContainer::TContainer(const string &name) : name(name), state(Stopped), spec(name) {
}

TContainer::TContainer(const std::string &name, const kv::TNode &node) : name(name), state(Stopped), spec(name, node) {
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

    vector<pid_t> ret;
    cg->GetProcesses(ret);
    return ret;
}

bool TContainer::IsAlive() {
    return IsRoot() || !Processes().empty();
}

void TContainer::UpdateState() {
    if (state == Running && !IsAlive())
        Stop();
}

TError TContainer::PrepareCgroups() {
    if (IsRoot()) {
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Memory()));
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Freezer()));
    } else {
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Memory())));
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer())));
    }

    for (auto cg : leaf_cgroups) {
        auto ret = cg->Create();
        if (ret) {
            leaf_cgroups.clear();
            return ret;
        }
    }

    return TError();
}

TError TContainer::Start()
{
    if (!CheckState(Stopped))
        return TError();

    auto ret = PrepareCgroups();
    if (ret)
        return ret;

    if (IsRoot())
        return ret;

    TTaskEnv taskEnv(spec.Get("command"), "");
    task = unique_ptr<TTask>(new TTask(taskEnv, leaf_cgroups));

    ret = task->Start();
    if (ret) {
        leaf_cgroups.clear();
        return ret;
    }

    state = Running;

    return ret;
}

static const auto kill_timeout = 100000;

TError TContainer::Stop()
{
    if (name == "/" || !CheckState(Running))
        return false;

    if (task->IsRunning())
        task->Kill(SIGTERM);
    task = nullptr;

    usleep(kill_timeout);

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));

    TSubsystem::Freezer()->Freeze(*cg);
    cg->Kill(SIGKILL);
    TSubsystem::Freezer()->Unfreeze(*cg);

    leaf_cgroups.clear();

    state = Stopped;

    return TError();
}

TError TContainer::Pause()
{
    if (name == "/" || !CheckState(Running))
        return false;

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Freeze(*cg);

    state = Paused;
    return true;
}

TError TContainer::Resume()
{
    if (!CheckState(Paused))
        return false;

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Unfreeze(*cg);

    state = Running;
    return true;
}

TError TContainer::GetData(string name, string &value)
{
    if (dataSpec.find(name) == dataSpec.end())
        return TError("No such data");

    value = dataSpec[name].Handler(*this);
    return TError();
}

TError TContainer::GetProperty(string property, string &value)
{
    value = spec.Get(property);
    return TError();
}

TError TContainer::SetProperty(string property, string value)
{
    if (name == "/")
        return false;

    //TODO: catch exception
    spec.Set(property, value);

    return true;
}

TError TContainer::Restore() {
    // TODO recover state, task, etc
    // probably need to PTRACE_SEIZE to be able to do waitpid ("reparent")

    PrepareCgroups();

    state = Stopped;
    task = nullptr;

    return TError();
}

// TContainerHolder
TContainerHolder::TContainerHolder() {
    if (Create("/"))
        throw "Cannot create root container";

    auto root = Get("/");
    root->Start();
}

TContainerHolder::~TContainerHolder() {
}

TError TContainerHolder::Create(string name) {
    if (containers[name] == nullptr) {
        containers[name] = make_shared<TContainer>(name);
        return TError();
    } else
        return TError("container " + name + " already exists");
}

shared_ptr<TContainer> TContainerHolder::Get(string name) {
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

TError TContainerHolder::Restore(const std::string &name, const kv::TNode &node)
{
    // TODO: we DO trust data from the persistent storage, do we?
    auto c = make_shared<TContainer>(name, node);
    auto e = c->Restore();
    if (e)
        return e;

    containers[name] = c;
    return TError();
}
