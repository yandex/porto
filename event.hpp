#ifndef __EVENT_H__
#define __EVENT_H__

#include <string>
#include <memory>
#include <queue>

class TContainer;

enum class EEventType {
    Exit,
    RotateLogs,
    Respawn,
};

class TEvent {
public:
    EEventType Type;

    struct {
        int Pid;
        int Status;
    } Exit;

    struct {
        std::weak_ptr<TContainer> Container;
    } Respawn;

    int TimeoutMs = 0;

    TEvent(int pid, int status) : Type(EEventType::Exit) {
        Exit.Pid = pid; Exit.Status = status;
    }
    TEvent() : Type(EEventType::RotateLogs) {
    }
    TEvent(const std::shared_ptr<TContainer> c) :
        Type(EEventType::Respawn) {
        Respawn.Container = c;
    }

    bool operator<(const TEvent& rhs) const;

    std::string GetMsg() const;
};

class TEventQueue {
    std::priority_queue<TEvent> Queue;

public:
    TEventQueue() {}

    void Add(size_t timeoutMs, TEvent &e);
    int GetNextTimeout();
};

#endif
