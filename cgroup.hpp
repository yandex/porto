#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <string>
#include <unordered_map>
#include <list>
#include <memory>

#include "common.hpp"

class TSubsystem;
class TMount;

class TCgroup : public std::enable_shared_from_this<TCgroup>,
                public TNonCopyable {
    const std::string Name;
    const std::shared_ptr<TCgroup> Parent;
    std::vector<std::weak_ptr<TCgroup>> Children;

    std::shared_ptr<TMount> Mount;

    mode_t Mode = 0755;

public:
    TCgroup(const std::vector<std::shared_ptr<TSubsystem>> subsystems,
            std::shared_ptr<TMount> m = nullptr);
    TCgroup(const std::string &name, const std::shared_ptr<TCgroup> parent) :
        Name(name), Parent(parent) {}

    ~TCgroup();

    std::shared_ptr<TCgroup> GetChild(const std::string& name);

    bool IsRoot() const;

    std::string Path() const;
    std::string Relpath() const;

    TError Create();
    TError Remove();
    bool Exists();
    std::shared_ptr<TMount> GetMount();

    TError Kill(int signal) const;

    TError FindChildren(std::vector<std::shared_ptr<TCgroup>> &cgroups);

    TError GetProcesses(std::vector<pid_t> &processes) const;
    TError GetTasks(std::vector<pid_t> &tasks) const;
    bool IsEmpty() const;

    TError Attach(int pid) const;

    bool HasKnob(const std::string &knob) const;
    TError GetKnobValue(const std::string &knob, std::string &value) const;
    TError GetKnobValueAsLines(const std::string &knob, std::vector<std::string> &lines) const;
    TError SetKnobValue(const std::string &knob, const std::string &value, bool append = false) const;
};

class TCgroupSnapshot : public TNonCopyable {
    std::vector<std::shared_ptr<TCgroup>> Cgroups;
public:
    TCgroupSnapshot() {}
    TError Create();
    void Destroy();
};

#endif
