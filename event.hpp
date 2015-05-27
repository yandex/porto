#pragma once

#include <string>
#include <memory>
#include <queue>

#include "util/worker.hpp"
#include "holder.hpp"

class TContainer;
class TContainerHolder;

enum class EEventType {
    Exit,
    RotateLogs,
    Respawn,
    OOM,
    CgroupSync,
};

class TEventWorker;

class TEvent {
public:
    EEventType Type;
    std::weak_ptr<TContainer> Container;
    bool Targeted;

    union {
        struct {
            int Pid;
            int Status;
        } Exit;

        struct {
            int Fd;
        } OOM;
    };

    size_t DueMs = 0;

    TEvent(EEventType type, std::shared_ptr<TContainer> container = nullptr) :
        Type(type), Container(container), Targeted(container != nullptr) {}

    bool operator<(const TEvent& rhs) const;

    std::string GetMsg() const;
};

class TEventQueue {
    std::priority_queue<TEvent> Queue;
    std::shared_ptr<TEventWorker> Worker;

public:
    TEventQueue(std::shared_ptr<TContainerHolder> holder);
    void Start();
    void Stop();

    void Add(size_t timeoutMs, const TEvent &e);
#if !THREADS
    void DeliverEvents(TContainerHolder &cholder);
    int GetNextTimeout();
#endif
};
