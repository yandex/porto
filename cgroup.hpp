#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <string>
#include <unordered_map>

#include "error.hpp"
#include "mount.hpp"
#include "folder.hpp"
#include "subsystem.hpp"

class TCgroup {
    std::string name;
    std::shared_ptr<TCgroup> parent;
    int level;
    vector<std::weak_ptr<TCgroup>> children;

    std::shared_ptr<TMount> mount;
    std::vector<std::shared_ptr<TSubsystem>> subsystems;

    std::string tmpfs = "/sys/fs/cgroup";
    mode_t mode = 0x666;

    bool need_cleanup = false;

public:
    static std::shared_ptr<TCgroup> Get(std::string name,
                                              std::shared_ptr<TCgroup> parent);
    static std::shared_ptr<TCgroup> GetRoot(std::shared_ptr<TMount> mount, std::vector<std::shared_ptr<TSubsystem>> subsystems);
    static std::shared_ptr<TCgroup> GetRoot(std::shared_ptr<TSubsystem> subsystem);

    TCgroup(std::string name, std::shared_ptr<TCgroup> parent, int level = 0);
    TCgroup(std::shared_ptr<TMount> mount, vector<std::shared_ptr<TSubsystem>> subsystems);
    TCgroup(std::vector<std::shared_ptr<TSubsystem>> controller);

    ~TCgroup();

    void SetNeedCleanup() {
        need_cleanup = true;
    }

    bool IsRoot() const;

    std::string Path();

    void Create();
    void Remove();

    void Kill(int signal);

    std::vector<std::shared_ptr<TCgroup> > FindChildren();

    std::vector<pid_t> Processes();
    std::vector<pid_t> Tasks();
    bool IsEmpty();

    TError Attach(int pid);

    std::string GetKnobValue(std::string knob);
    void SetKnobValue(std::string knob, std::string value, bool append = false);

    friend bool operator==(const TCgroup& c1, const TCgroup& c2);
    friend ostream& operator<<(ostream& os, const TCgroup& cg);
};

class TCgroupSnapshot {
    std::vector<std::shared_ptr<TCgroup> > cgroups;
    std::unordered_map<string, std::shared_ptr<TSubsystem> > subsystems; // can be net_cls _or_ net_prio
public:
    TCgroupSnapshot();

    friend ostream& operator<<(ostream& os, const TCgroupSnapshot& st);
};

#endif
