#ifndef __CONTEXT_H__
#define __CONTEXT_H__

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

    TContext();
    TError Initialize();
    TError Destroy();
};

#endif /* __CONTEXT_H__ */
