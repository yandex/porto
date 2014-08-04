#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <set>
#include <string>
#include <map>

#include "error.hpp"
#include "mount.hpp"
#include "folder.hpp"

class TRootCgroup;
class TCgroup {
protected:
    string name;
private:
    std::shared_ptr<TCgroup> parent;
    int level;
    vector<std::weak_ptr<TCgroup> > children;

    mode_t mode = 0x666;

public:
    TCgroup(string name, std::shared_ptr<TCgroup> parent, int level = 0);
    virtual ~TCgroup();

    void FindChildren();

    string Name();
    virtual string Path();

    void Create();
    void Remove();

    TError Attach(int pid);

    friend bool operator==(const TCgroup& c1, const TCgroup& c2) {
        return c1.name == c2.name && *c1.parent == *c2.parent;
    }

    friend ostream& operator<<(ostream& os, const TCgroup& cg);
};

class TController {
    string name;

public:
    TController(string name);
    string Name();
};

class TRootCgroup : public TCgroup {
    std::shared_ptr<TMount> mount;
    std::set<TController*> controllers;

    mode_t mode = 0x666;
    string tmpfs = "/sys/fs/cgroup";

public:
    TRootCgroup(std::shared_ptr<TMount> mount, set<TController*> controllers);
    TRootCgroup(set<TController*> controller);
    virtual ~TRootCgroup();

    virtual string Path();

    void Mount();
    void Detach();
};

class TCgroupState {
    map<string, std::shared_ptr<TRootCgroup> > root_cgroups; // can be net_cls,netprio
    map<string, TController*> controllers; // can be net_cls _or_ net_prio

public:
    TCgroupState();
    ~TCgroupState();

    /*
    void MountMissingTmpfs(string tmpfs = "/sys/fs/cgroup");
    void MountMissingControllers();
    void UmountAll();
    */

    friend ostream& operator<<(ostream& os, const TCgroupState& st);
};

#endif
