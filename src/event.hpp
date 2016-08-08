#pragma once

#include <string>
#include <memory>

#include "util/worker.hpp"

class TContainer;
class TContainerHolder;
class TContainerWaiter;

enum class EEventType {
    Exit,
    RotateLogs,
    Respawn,
    OOM,
    WaitTimeout,
    UpdateNetwork,
    DestroyWeak,
};

class TEventWorker;

class TEvent {
public:
    EEventType Type;
    std::weak_ptr<TContainer> Container;

    struct {
        int Pid;
        int Status;
    } Exit;

    struct {
        int Fd;
    } OOM;

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
    TEventQueue(std::shared_ptr<TContainerHolder> holder);
    void Start();
    void Stop();

    void Add(uint64_t timeoutMs, const TEvent &e);
};
