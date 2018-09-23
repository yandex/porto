#pragma once

#include <string>
#include <memory>

#include "util/worker.hpp"

class TContainer;
class TContainerWaiter;

enum class EEventType {
    Exit,
    ChildExit,
    RotateLogs,
    Respawn,
    OOM,
    WaitTimeout,
    DestroyAgedContainer,
    DestroyWeakContainer,
};

class TEventWorker;

class TEvent {
public:
    EEventType Type;
    std::weak_ptr<TContainer> Container;

    struct {
        int Pid = 0;
        int Status = 0;
    } Exit;

    struct {
        std::weak_ptr<TContainerWaiter> Waiter;
    } WaitTimeout;

    uint64_t DueMs = 0;

    TEvent(EEventType type, std::shared_ptr<TContainer> container = nullptr) :
        Type(type), Container(container) {}

    bool operator<(const TEvent& rhs) const;

    std::string GetMsg() const;
};

class TEventQueue {
    std::shared_ptr<TEventWorker> Worker;

public:
    TEventQueue();
    void Start();
    void Stop();

    void Add(uint64_t timeoutMs, const TEvent &e);
};
