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

class TContainerHolder {
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

    TContainerHolder(std::shared_ptr<TEventQueue> queue,
                     std::shared_ptr<TNetwork> net) :
        Net(net), Queue(queue) { }
    ~TContainerHolder();
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
