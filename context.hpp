#ifndef __CONTEXT_H__
#define __CONTEXT_H_

#include "event.hpp"
#include "holder.hpp"
#include "qdisc.hpp"

class TContext {
public:
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TEventQueue> Queue;
    std::shared_ptr<TNetwork> Net;
    std::shared_ptr<TNl> NetEvt;
    std::shared_ptr<TContainerHolder> Cholder;
    std::shared_ptr<TVolumeHolder> Vholder;

    TContext() {
        Storage = std::make_shared<TKeyValueStorage>();
        Queue = std::make_shared<TEventQueue>();
        Net = std::make_shared<TNetwork>();
        Cholder = std::make_shared<TContainerHolder>(Queue, Net);
        Vholder = std::make_shared<TVolumeHolder>();
    }

    TError Initialize();
    TError Destroy();
};

#endif /* __CONTEXT_H__ */
