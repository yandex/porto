#ifndef __CGROUP_HPP__
#define __CGROUP_HPP__

#include <set>
#include <string>
#include <map>

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
    TCgroup(string name, std::shared_ptr<TCgroup> parent, int level);
    virtual ~TCgroup();

    void FindChildren();
    void DropChildren();

    string Name();
    virtual string Path();

    void Create();
    void Remove();

    int Attach(int pid);
    
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

    void Attach();
    void Detach();
};

class TCgroupState {
    map<string, std::shared_ptr<TRootCgroup> > root_cgroups; // can be net_cls,netprio
    map<string, TController*> controllers; // can be net_cls _or_ net_prio

public:
    TCgroupState();
    ~TCgroupState();

    void MountMissingTmpfs(string tmpfs = "/sys/fs/cgroup");
    void MountMissingControllers();
    void UmountAll();

    friend ostream& operator<<(ostream& os, const TCgroupState& st);
};

class TCgroupRegistry {
    std::vector<std::weak_ptr<TCgroup>> cgroups;

    std::shared_ptr<TCgroup> GetCgroup(std::string name,
                                       std::shared_ptr<TCgroup> parent);
    std::shared_ptr<TRootCgroup> GetRootCgroup();
};

#endif
