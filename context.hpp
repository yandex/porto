#ifndef __CONTEXT_H__
#define __CONTEXT_H__

#include "event.hpp"
#include "holder.hpp"
#include "qdisc.hpp"
#include "volumeAPI/volume.hpp"
#include "util/mount.hpp"
#include "config.hpp"

typedef std::function<TError()> task_t;
typedef std::function<void(int ret)> posthook_t;

class TEpollLoop;

class TContext : public TNonCopyable {
public:
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TKeyValueStorage> VolumeStorage;
    std::shared_ptr<TEventQueue> Queue;
    std::shared_ptr<TNetwork> Net;
    std::shared_ptr<TNl> NetEvt;
    std::shared_ptr<TContainerHolder> Cholder;
    std::shared_ptr<TVolumeHolder> Vholder;
    std::shared_ptr<TEpollLoop> EpollLoop;

    std::map<pid_t, posthook_t> Posthooks;
    std::map<pid_t, int> Errors;

    TContext();
    TError Initialize();
    TError Destroy();
};

#endif /* __CONTEXT_H__ */
