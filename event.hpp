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

    struct {
        int Pid;
        int Status;
    } Exit;

    struct {
        std::weak_ptr<TContainer> Container;
    } Respawn;

    struct {
        int Fd;
    } OOM;

    size_t DueMs = 0;

    TEvent(int pid, int status) : Type(EEventType::Exit) {
        Exit.Pid = pid; Exit.Status = status;
    }
    TEvent() : Type(EEventType::RotateLogs) {
    }
    TEvent(const std::shared_ptr<TContainer> c) :
        Type(EEventType::Respawn) {
        Respawn.Container = c;
    }
    TEvent(int fd) : Type(EEventType::OOM) {
        OOM.Fd = fd;
    }

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
