#pragma once

#include <memory>

#include "common.hpp"

class TEpollLoop;
class TCgroup;
class TSubsystem;
class TKeyValueStorage;
class TEventQueue;
class TContainerHolder;
class TVolumeHolder;

class TContext : public TNonCopyable {
public:
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TKeyValueStorage> VolumeStorage;
    std::shared_ptr<TEventQueue> Queue;
    std::shared_ptr<TContainerHolder> Cholder;
    std::shared_ptr<TVolumeHolder> Vholder;
    std::shared_ptr<TEpollLoop> EpollLoop;

    TContext();
    TError Initialize();
    TError Destroy();
};
