#pragma once

#include <vector>
#include <map>
#include <memory>
#include <mutex>

#include "common.hpp"
#include "kvalue.hpp"
#include "util/idmap.hpp"
#include "util/cred.hpp"

class TNetwork;
class TContainer;
class TIdMap;
class TEventQueue;
class TEvent;
class TEpollLoop;

class TContainerHolder : public std::enable_shared_from_this<TContainerHolder>,
                         public TLockable<> {
    std::shared_ptr<TNetwork> Net;
    std::map<std::string, std::shared_ptr<TContainer>> Containers;
    TIdMap IdMap;
    std::shared_ptr<TKeyValueStorage> Storage;

    bool ValidName(const std::string &name) const;
    TError RestoreId(const kv::TNode &node, uint16_t &id);
    void ScheduleLogRotatation();
    void ScheduleCgroupSync();
    TError _Destroy(const std::string &name);
    TError ReserveDefaultClassId();
    std::map<std::string, std::shared_ptr<TKeyValueNode>>
        SortNodes(const std::vector<std::shared_ptr<TKeyValueNode>> &nodes);

public:
    std::shared_ptr<TEventQueue> Queue = nullptr;
    std::shared_ptr<TEpollLoop> EpollLoop;

    TContainerHolder(std::shared_ptr<TEpollLoop> epollLoop,
                     std::shared_ptr<TNetwork> net,
                     std::shared_ptr<TKeyValueStorage> storage) :
        Net(net), Storage(storage), EpollLoop(epollLoop) { }
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot();
    TError CreatePortoRoot();
    TError Create(const std::string &name, const TCred &cred);
    TError Get(const std::string &name, std::shared_ptr<TContainer> &c);
    TError Get(int pid, std::shared_ptr<TContainer> &c);
    TError Restore(const std::string &name, const kv::TNode &node);
    bool RestoreFromStorage();
    TError Destroy(const std::string &name);
    void DestroyRoot();

    std::vector<std::shared_ptr<TContainer> > List() const;

    bool DeliverEvent(const TEvent &event);
};
