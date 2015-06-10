#pragma once

#include "event.hpp"
#include "holder.hpp"
#include "qdisc.hpp"
#include "volume.hpp"
#include "util/mount.hpp"
#include "config.hpp"

class TEpollLoop;
class TCgroup;
class TSubsystem;

class TContext : public TNonCopyable {
    TError CreateDaemonCgs();

public:
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TKeyValueStorage> VolumeStorage;
    std::shared_ptr<TEventQueue> Queue;
    std::shared_ptr<TNetwork> Net;
    std::shared_ptr<TNl> NetEvt;
    std::shared_ptr<TContainerHolder> Cholder;
    std::shared_ptr<TVolumeHolder> Vholder;
    std::shared_ptr<TEpollLoop> EpollLoop;
    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> DaemonCgs;

    TContext();
    TError Initialize();
    TError Destroy();
};
