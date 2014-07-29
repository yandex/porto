#include <set>
#include <string>

#include "mount.hpp"

class TCgroup {
    string name;
    TCgroup *parent;
    int level;
    set<TCgroup*> children;

    mode_t mode = 0x666;

public:
    TCgroup(string path, TCgroup *parent = nullptr, int level = 0) :
        name (path), parent(parent), level(level) {}

    void FindChildren();

    void DropChildren() {
        for (auto c : children)
            delete c;
    }

    ~TCgroup() {
        DropChildren();
    }

    string Name() {
        return name;
    }

    void Create();
    void Remove();
    
    friend ostream& operator<<(ostream& os, const TCgroup& cg);
};

class TCgroupState {
    // cgroups are owned by controllers, not raw_controllers
    map<string, TCgroup*> controllers; // can be net_cls,netprio
    map<string, TCgroup*> raw_controllers; // can be net_cls _or_ net_prio

public:
    TCgroupState();

    ~TCgroupState() {
        for (auto c : controllers)
            delete c.second;
    }

    friend ostream& operator<<(ostream& os, const TCgroupState& st) {
        for (auto ss : st.controllers) {
            os << ss.first << ":" << endl;
            os << *ss.second << endl;
        }

        return os;
    }

    const string DefaultMountpoint(const string controller) {
        return "/sys/fs/cgroup/" + controller;
    }

    void Update();
    void MountMissingControllers();
    void UmountAll();
};
