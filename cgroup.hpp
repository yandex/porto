#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <set>
#include <string>

#include "mount.hpp"
#include "folder.hpp"

class TController {
    string name;
    TMount *mount;

    mode_t mode = 0x666;
    string tmpfs = "/sys/fs/cgroup"; // TODO: detect dynamically

public:
    TController(string name, TMount *mount) : name(name), mount(mount) {}
    TController(string name) {
        string path = tmpfs + "/" + name;
        mount = new TMount("cgroup", path, "cgroup", 0, set<string>{name});
    }

    void Attach() {
        TFolder f(mount->Mountpoint());
        if (!f.Exists())
            f.Create(mode);
        mount->Mount();
    }

    void Detach() {
        TFolder f(mount->Mountpoint());
        mount->Umount();
        f.Remove();
    }
};

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
    map<string, TCgroup*> root_cgroups; // can be net_cls,netprio
    map<string, TController*> controllers; // can be net_cls _or_ net_prio

public:
    ~TCgroupState() {
        for (auto c : root_cgroups)
            delete c.second;
    }

    friend ostream& operator<<(ostream& os, const TCgroupState& st) {
        for (auto ss : st.root_cgroups) {
            os << ss.first << ":" << endl;
            os << *ss.second << endl;
        }

        return os;
    }

    void UpdateFromProcFs();

    void MountMissingTmpfs(string tmpfs = "/sys/fs/cgroup");
    void MountMissingControllers();
    void UmountAll();
};

#endif
