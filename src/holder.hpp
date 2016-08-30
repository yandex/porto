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
class TKeyValue;
class TNetwork;

class TContainerHolder : public std::enable_shared_from_this<TContainerHolder> {
    TIdMap IdMap;

    void ScheduleLogRotatation();
    void Unlink(TScopedLock &holder_lock, std::shared_ptr<TContainer> c);

public:
    std::shared_ptr<TEventQueue> Queue = nullptr;
    std::shared_ptr<TEpollLoop> EpollLoop;

    TContainerHolder(std::shared_ptr<TEpollLoop> epollLoop) :
        IdMap(1, CONTAINER_ID_MAX), EpollLoop(epollLoop) { }
    TError ValidName(const std::string &name) const;
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot(TScopedLock &holder_lock);
    TError CreatePortoRoot(TScopedLock &holder_lock);
    TError Create(TScopedLock &holder_lock, const std::string &name, std::shared_ptr<TContainer> &container);
    TError Get(const std::string &name, std::shared_ptr<TContainer> &c);
    TError FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &c);

    TError GetLocked(TScopedLock &holder_lock,
                     const TClient *client,
                     const std::string &name,
                     const bool checkPerm,
                     std::shared_ptr<TContainer> &c,
                     TNestedScopedLock &l);

    TError Restore(TScopedLock &holder_lock, TKeyValue &node);
    bool RestoreFromStorage();
    void RemoveLeftovers();
    TError Destroy(TScopedLock &holder_lock, std::shared_ptr<TContainer> c);
    void DestroyRoot(TScopedLock &holder_lock);

    std::vector<std::shared_ptr<TContainer> > List(bool all = false) const;

    bool DeliverEvent(const TEvent &event);
};
