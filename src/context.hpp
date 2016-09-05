#pragma once

#include <memory>

#include "common.hpp"

class TEpollLoop;
class TCgroup;
class TSubsystem;
class TEventQueue;
class TContainerHolder;

class TContext : public TNonCopyable {
public:
    std::shared_ptr<TEventQueue> Queue;
    std::shared_ptr<TContainerHolder> Cholder;
    std::shared_ptr<TEpollLoop> EpollLoop;

    TContext();
    TError Initialize();
    TError Destroy();
};
