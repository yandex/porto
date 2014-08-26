#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <string>
#include <unordered_map>
#include <list>
#include <memory>

#include "porto.hpp"
#include "error.hpp"
#include "util/mount.hpp"
#include "util/folder.hpp"

class TSubsystem;

class TCgroup : public std::enable_shared_from_this<TCgroup> {
    const std::string name;
    const std::shared_ptr<TCgroup> parent;
    std::vector<std::weak_ptr<TCgroup>> children;

    std::shared_ptr<TMount> mount;

    std::string tmpfs = "/sys/fs/cgroup";
    mode_t mode = 0755;

    bool need_cleanup = false;

public:
    TCgroup(const std::vector<std::shared_ptr<TSubsystem>> controller,
            std::shared_ptr<TMount> mount = nullptr);
    TCgroup(const std::string &name, std::shared_ptr<TCgroup> parent) :
        name(name), parent(parent) {}

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
};

class TCgroupSnapshot {
    std::vector<std::shared_ptr<TCgroup> > cgroups;
    std::unordered_map<std::string, std::shared_ptr<TSubsystem>> subsystems; // can be net_cls _or_ net_prio
public:
    TError Create();
};

#endif
