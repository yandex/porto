#include "config.hpp"
#include "event.hpp"
#include "container.hpp"

std::string TEvent::GetMsg() const {
    switch (Type) {
        case EEventType::Exit:
            return "exit status " + std::to_string(Exit.Status)
                + " for pid " + std::to_string(Exit.Pid);
        case EEventType::RotateLogs:
            return "rotate logs";
        case EEventType::Respawn:
            return "respawn";
        default:
            return "unknown event";
    }
}

bool TEvent::operator<(const TEvent& rhs) const {
    return TimeoutMs >= rhs.TimeoutMs;
}

void TEventQueue::Add(size_t timeoutMs, TEvent &e) {
    e.TimeoutMs = timeoutMs;
    Queue.push(e);
}

int TEventQueue::GetNextTimeout() {
    if (Queue.empty())
        return config().daemon().poll_timeout_ms();
    else
        return Queue.top().TimeoutMs;
}
