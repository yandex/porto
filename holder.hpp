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
class TContext;

class TContainerHolder : public std::enable_shared_from_this<TContainerHolder> {
    std::shared_ptr<TNetwork> Net;
    std::map<std::string, std::shared_ptr<TContainer>> Containers;
    TIdMap IdMap;

    bool ValidName(const std::string &name) const;
    TError RestoreId(const kv::TNode &node, uint16_t &id);
    void ScheduleLogRotatation();
    TError _Destroy(const std::string &name);
    std::shared_ptr<TKeyValueStorage> Storage;

public:
    std::shared_ptr<TEventQueue> Queue;
    TContext *Context;

    TContainerHolder(TContext *context,
                     std::shared_ptr<TEventQueue> queue,
                     std::shared_ptr<TNetwork> net,
                     std::shared_ptr<TKeyValueStorage> storage) :
        Net(net), Storage(storage), Queue(queue), Context(context) { }
    std::shared_ptr<TContainer> GetParent(const std::string &name) const;
    TError CreateRoot();
    TError Create(const std::string &name, const TCred &cred);
    std::shared_ptr<TContainer> Get(const std::string &name);
    TError Restore(const std::string &name, const kv::TNode &node);
    TError Destroy(const std::string &name);
    void DestroyRoot();

    std::vector<std::string> List() const;

    bool DeliverEvent(const TEvent &event);
};

#endif
