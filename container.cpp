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
            return "-1";
    };

    static string ExitStatus(TContainer& c) {
        if (c.task) {
            TExitStatus status = c.task->GetExitStatus();
            stringstream ss;
            ss << status.error << ";" << status.signal << ";" << status.status;
            return ss.str();
        }
        else
            return "-1";
    };

    static string Stdout(TContainer& c) {
        if (c.task)
            return c.task->GetStdout();
        return "";
    };

    static string Stderr(TContainer& c) {
        if (c.task)
            return c.task->GetStderr();
        return "";
    };

    static string CpuUsage(TContainer& c) {
        auto subsys = TSubsystem::Cpuacct();
        auto cg = c.GetCgroup(subsys);
        if (!cg) {
            TLogger::LogAction("cpuacct cgroup not found");
            return "-1";
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
        if (error) {
            TLogger::LogError(error);
            return "-1";
        }

        return to_string(val);
    };

    static string MemUsage(TContainer& c) {
        auto subsys = TSubsystem::Memory();
        auto cg = c.GetCgroup(subsys);
        if (!cg) {
            TLogger::LogAction("memory cgroup not found");
            return "-1";
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
        if (error) {
            TLogger::LogError(error);
            return "-1";
        }

        return to_string(val);
    };
};

std::map<std::string, const TDataSpec> dataSpec = {
    { "state", { "container state", TData::State } },
    { "exit_status", { "container exit status", TData::ExitStatus } },
    { "root_pid", { "root process id", TData::RootPid } },
    { "stdout", { "return task stdout", TData::Stdout } },
    { "stderr", { "return task stderr", TData::Stderr } },
    { "cpu_usage", { "return consumed CPU time in nanoseconds", TData::CpuUsage } },
    { "mem_usage", { "return consumed memory in bytes", TData::MemUsage } },
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

TContainer::~TContainer() {
    if (state == Paused)
        Resume();

    Stop();
}

string TContainer::Name() {
    return name;
}

bool TContainer::IsRoot() {
    return name == RootName;
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
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Cpuacct()));
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Memory()));
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Freezer()));
    } else {
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Cpuacct())));
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

    return NoError;
}

TError TContainer::Start() {
    if (!CheckState(Stopped))
        return NoError;

    auto ret = PrepareCgroups();
    if (ret)
        return ret;

    if (IsRoot()) {
        state = Running;
        return ret;
    }

    TTaskEnv taskEnv(spec.Get("command"), "", spec.Get("user"), spec.Get("group"), spec.Get("env"));
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

TError TContainer::Stop() {
    if (IsRoot() || !CheckState(Running))
        return TError(TError::BadValue);

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

    return NoError;
}

TError TContainer::Pause() {
    if (IsRoot() || !CheckState(Running))
        return TError(TError::BadValue);

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Freeze(*cg);

    state = Paused;
    return NoError;
}

TError TContainer::Resume() {
    if (!CheckState(Paused))
        return TError(TError::BadValue);

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Unfreeze(*cg);

    state = Running;
    return NoError;
}

TError TContainer::GetData(const string &name, string &value) {
    if (dataSpec.find(name) == dataSpec.end())
        return TError(TError::BadValue);

    value = dataSpec[name].Handler(*this);
    return NoError;
}

TError TContainer::GetProperty(const string &property, string &value) {
    value = spec.Get(property);
    return NoError;
}

TError TContainer::SetProperty(const string &property, const string &value) {
    if (IsRoot())
        return TError(TError::BadValue, "Can't set property for root");

    if (task && task->IsRunning() && !spec.IsDynamic(property))
        return TError(TError::BadValue, "Can't set dynamic property " + property + " for running container");

    return spec.Set(property, value);
}

TError TContainer::Restore() {
    // TODO recover state, task, etc
    // probably need to PTRACE_SEIZE to be able to do waitpid ("reparent")

    PrepareCgroups();

    state = IsAlive() ? Running : Stopped;
    task = nullptr;

    return NoError;
}

std::shared_ptr<TCgroup> TContainer::GetCgroup(shared_ptr<TSubsystem> subsys) {
    return TCgroup::Get(name, TCgroup::GetRoot(subsys));
}

// TContainerHolder
TContainerHolder::TContainerHolder() {
    if (Create(RootName))
        throw "Cannot create root container";

    auto root = Get(RootName);
    root->Start();
}

TContainerHolder::~TContainerHolder() {
}

bool TContainerHolder::ValidName(const string &name) {
    if (name == RootName)
        return true;

    return find_if(name.begin(), name.end(),
                   [](const char c) -> bool {
                        return !(isalnum(c) || c == '_');
                   }) == name.end();
}

TError TContainerHolder::Create(const string &name) {
    if (!ValidName(name))
        return TError(TError::BadValue, "invalid container name " + name);

    if (containers[name] == nullptr) {
        containers[name] = make_shared<TContainer>(name);
        return NoError;
    } else
        return TError(TError::BadValue, "container " + name + " already exists");
}

shared_ptr<TContainer> TContainerHolder::Get(const string &name) {
    if (containers.find(name) == containers.end())
        return shared_ptr<TContainer>();

    return containers[name];
}

void TContainerHolder::Destroy(const string &name) {
    if (name != RootName)
        containers.erase(name);
}

vector<string> TContainerHolder::List() {
    vector<string> ret;

    for (auto c : containers)
        ret.push_back(c.second->Name());

    return ret;
}

TError TContainerHolder::Restore(const std::string &name, const kv::TNode &node) {
    // TODO: we DO trust data from the persistent storage, do we?
    auto c = make_shared<TContainer>(name, node);
    auto e = c->Restore();
    if (e)
        return e;

    containers[name] = c;
    return NoError;
}
