#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <set>
#include <string>
#include <map>

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
    std::set<std::shared_ptr<TSubsystem>> subsystems;

    std::string tmpfs = "/sys/fs/cgroup";
    mode_t mode = 0x666;

public:
    TCgroup(std::string name, std::shared_ptr<TCgroup> parent, int level = 0);
    TCgroup(std::shared_ptr<TMount> mount, set<std::shared_ptr<TSubsystem>> subsystems);
    TCgroup(set<std::shared_ptr<TSubsystem>> controller);

    ~TCgroup();

    std::vector<std::shared_ptr<TCgroup> > FindChildren();

    std::string Name();
    std::string Path();

    void Create();
    void Remove();

    bool IsRoot() const;

    TError Attach(int pid);

    friend bool operator==(const TCgroup& c1, const TCgroup& c2) {
        if (c1.name != c2.name)
            return false;
        if (c1.parent != c2.parent)
            return false;
        if (!c1.parent && !c2.parent)
            return c1.subsystems == c2.subsystems;
        return true;
    }

    friend ostream& operator<<(ostream& os, const TCgroup& cg);
};

class TCgroupSnapshot {
    std::vector<std::shared_ptr<TCgroup> > cgroups;
    std::map<string, std::shared_ptr<TSubsystem> > subsystems; // can be net_cls _or_ net_prio
public:
    TCgroupSnapshot();

    friend ostream& operator<<(ostream& os, const TCgroupSnapshot& st);
};

#endif
