#ifndef __CONTEXT_H__
#define __CONTEXT_H__

#include "event.hpp"
#include "holder.hpp"
#include "qdisc.hpp"
#include "volumeAPI/volume.hpp"
#include "util/mount.hpp"
#include "config.hpp"

class TContext {
public:
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TKeyValueStorage> VolumeStorage;
    std::shared_ptr<TEventQueue> Queue;
    std::shared_ptr<TNetwork> Net;
    std::shared_ptr<TNl> NetEvt;
    std::shared_ptr<TContainerHolder> Cholder;
    std::shared_ptr<TVolumeHolder> Vholder;

    int Epfd;

    TContext();
    TError Initialize();
    TError Destroy();
};

#endif /* __CONTEXT_H__ */
