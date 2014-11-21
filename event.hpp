#ifndef __EVENT_H__
#define __EVENT_H__

#include <string>
#include <memory>
#include <queue>

class TContainer;
class TContainerHolder;

enum class EEventType {
    Exit,
    RotateLogs,
    Respawn,
    OOM,
};

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

    size_t DueMs = 0;

    TEvent(EEventType type) : Type(type) {}

    bool operator<(const TEvent& rhs) const;

    std::string GetMsg() const;
};

class TEventQueue {
    std::priority_queue<TEvent> Queue;

public:
    TEventQueue() {}

    void Add(size_t timeoutMs, const TEvent &e);
    void DeliverEvents(TContainerHolder &cholder);
    int GetNextTimeout();
};

#endif
