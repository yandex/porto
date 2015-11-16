#pragma once

#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common.hpp"
#include "util/idmap.hpp"
#include "util/cred.hpp"
#include "util/locks.hpp"

class TContainer;
class TIdMap;
class TEventQueue;
class TEvent;
class TEpollLoop;
class TClient;
class TContainer;
class TKeyValueStorage;
class TKeyValueNode;
class TNetwork;

namespace kv {
    class TNode;
};

class TContainerHolder : public std::enable_shared_from_this<TContainerHolder>,
                         public TLockable {
    std::map<std::string, std::shared_ptr<TContainer>> Containers;
    TIdMap IdMap;
    std::shared_ptr<TKeyValueStorage> Storage;

    TError RestoreId(const kv::TNode &node, uint16_t &id);
    void ScheduleLogRotatation();
    void ScheduleCgroupSync();
    TError ReserveDefaultClassId();
    std::map<std::string, std::shared_ptr<TKeyValueNode>>
        SortNodes(const std::vector<std::shared_ptr<TKeyValueNode>> &nodes);
    void Unlink(TScopedLock &holder_lock, std::shared_ptr<TContainer> c);

public:
    std::shared_ptr<TEventQueue> Queue = nullptr;
    std::shared_ptr<TEpollLoop> EpollLoop;
    std::unordered_map<ino_t, std::weak_ptr<TNetwork>> NetNsMap;

    TContainerHolder(std::shared_ptr<TEpollLoop> epollLoop,
                     std::shared_ptr<TKeyValueStorage> storage) :
        Storage(storage), EpollLoop(epollLoop) { }
    TError ValidName(const std::string &name) const;
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot(TScopedLock &holder_lock);
    TError CreatePortoRoot(TScopedLock &holder_lock);
    TError Create(TScopedLock &holder_lock, const std::string &name, const TCred &cred, std::shared_ptr<TContainer> &container);
    TError Get(const std::string &name, std::shared_ptr<TContainer> &c);
    TError Get(int pid, std::shared_ptr<TContainer> &c);

    TError GetLocked(TScopedLock &holder_lock,
                     const std::shared_ptr<TClient> client,
                     const std::string &name,
                     const bool checkPerm,
                     std::shared_ptr<TContainer> &c,
                     TNestedScopedLock &l);

    TError Restore(TScopedLock &holder_lock, const std::string &name,
                   const kv::TNode &node);
    bool RestoreFromStorage();
    TError Destroy(TScopedLock &holder_lock, std::shared_ptr<TContainer> c);
    void DestroyRoot(TScopedLock &holder_lock);

    std::vector<std::shared_ptr<TContainer> > List(bool all = false) const;

    bool DeliverEvent(const TEvent &event);
};
