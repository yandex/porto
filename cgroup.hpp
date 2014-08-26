#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <string>
#include <unordered_map>
#include <list>
#include <memory>

#include "porto.hpp"
#include "error.hpp"
#include "subsystem.hpp"
#include "util/mount.hpp"
#include "util/folder.hpp"

class TCgroupRegistry;

class TCgroup : public std::enable_shared_from_this<TCgroup> {
    const std::string name;
    const std::shared_ptr<TCgroup> parent;
    std::vector<std::weak_ptr<TCgroup>> children;

    std::shared_ptr<TMount> mount;
    std::vector<std::shared_ptr<TSubsystem>> subsystems;

    std::string tmpfs = "/sys/fs/cgroup";
    mode_t mode = 0755;

    bool need_cleanup = false;

    friend TCgroupRegistry;
    TCgroup(const std::shared_ptr<TMount> mount, const std::vector<std::shared_ptr<TSubsystem>> subsystems) :
        name("/"), parent(std::shared_ptr<TCgroup>(nullptr)), mount(mount), subsystems(subsystems) { }

    TCgroup(const std::vector<std::shared_ptr<TSubsystem>> controller);
    
public:
    TCgroup(const std::string &name, std::shared_ptr<TCgroup> parent) :
        name(name), parent(parent) { }
    ~TCgroup();

    std::shared_ptr<TCgroup> GetChild(const std::string& name);

    void SetNeedCleanup() {
        need_cleanup = true;
    }

    bool IsRoot() const;

    std::string Path();
    std::string Relpath();

    TError Create();
    TError Remove();

    TError Kill(int signal);

    TError FindChildren(std::vector<std::shared_ptr<TCgroup>> cgroups);

    TError GetProcesses(std::vector<pid_t> &processes);
    TError GetTasks(std::vector<pid_t> &tasks);
    bool IsEmpty();

    TError Attach(int pid);

    bool HasKnob(const std::string &knob);
    TError GetKnobValue(const std::string &knob, std::string &value);
    TError GetKnobValueAsLines(const std::string &knob, std::vector<std::string> &lines);
    TError SetKnobValue(const std::string &knob, const std::string &value, bool append = false);
    bool HasSubsystem(const std::string &name);

    friend bool operator==(const TCgroup& c1, const TCgroup& c2);
};

class TCgroupSnapshot {
    std::vector<std::shared_ptr<TCgroup> > cgroups;
    std::unordered_map<std::string, std::shared_ptr<TSubsystem>> subsystems; // can be net_cls _or_ net_prio
public:
    TError Create();
};

class TCgroupRegistry {
    std::list<std::weak_ptr<TCgroup>> items;

    TCgroupRegistry() {};
    TCgroupRegistry(TCgroupRegistry const&) = delete;
    void operator=(TCgroupRegistry const&) = delete;

    static TCgroupRegistry &GetInstance() {
        static TCgroupRegistry instance;
        return instance;
    }

    std::shared_ptr<TCgroup> GetItem(const TCgroup &item) {
        items.remove_if([] (std::weak_ptr<TCgroup> i) {
                return i.expired();
            });

        for (auto i : items) {
            if (auto il = i.lock()) {
                if (item == *il)
                    return il;
            }
        }

        auto n = std::make_shared<TCgroup>(item);
        items.push_back(n);
        n->SetNeedCleanup();

        return n;
    }

public:
    static std::shared_ptr<TCgroup> GetRoot(const std::shared_ptr<TMount> mount,
                                            const std::vector<std::shared_ptr<TSubsystem>> subsystems);
    static std::shared_ptr<TCgroup> GetRoot(const std::shared_ptr<TSubsystem> subsystem);
};

#endif
