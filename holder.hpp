#ifndef __HOLDER_H__
#define __HOLDER_H__

#include <vector>
#include <map>
#include <memory>

#include "common.hpp"
#include "kvalue.hpp"
#include "util/idmap.hpp"
#include "util/cred.hpp"

class TNetwork;
class TContainer;
class TIdMap;
class TEventQueue;
class TEvent;

class TCredAdmin : public TNonCopyable {
private:
    std::set<int> PrivilegedUid, PrivilegedGid;
    std::set<int> RestrictedRootUid, RestrictedRootGid;
public:
    TError Initialize();
    bool PrivilegedUser(const TCred &cred);
    bool RestrictedUser(const TCred &cred);
    TError CheckPermission(std::shared_ptr<TContainer> container, const TCred &cred);
};

class THolder : public TCredAdmin {
    std::shared_ptr<TNetwork> Net;
    std::map<std::string, std::shared_ptr<TContainer>> Containers;
    TIdMap IdMap;

    bool ValidName(const std::string &name) const;
    TError RestoreId(const kv::TNode &node, uint16_t &id);
    void ScheduleLogRotatation();
    TError _Destroy(const std::string &name);
public:
    std::shared_ptr<TEventQueue> Queue;
    int Epfd;

    THolder(std::shared_ptr<TEventQueue> queue,
                     std::shared_ptr<TNetwork> net) :
        Net(net), Queue(queue) { }
    ~THolder();
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot();
    TError Create(const std::string &name, const TCred &cred);
    std::shared_ptr<TContainer> Get(const std::string &name);
    TError Restore(const std::string &name, const kv::TNode &node);
    TError Destroy(const std::string &name);

    std::vector<std::string> List() const;

    bool DeliverEvent(const TEvent &event);
};

#endif
