#include "config.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"

std::string TEvent::GetMsg() const {
    switch (Type) {
        case EEventType::Exit:
            return "exit status " + std::to_string(Exit.Status)
                + " for pid " + std::to_string(Exit.Pid);
        case EEventType::RotateLogs:
            return "rotate logs";
        case EEventType::Respawn:
            return "respawn";
        case EEventType::OOM:
            return "OOM killed with fd " + std::to_string(OOM.Fd);
        default:
            return "unknown event";
    }
}

bool TEvent::operator<(const TEvent& rhs) const {
    return DueMs >= rhs.DueMs;
}

void TEventQueue::Add(size_t timeoutMs, const TEvent &e) {
    TEvent copy = e;
    copy.DueMs = GetCurrentTimeMs() + timeoutMs;

    if (config().log().verbose())
        L() << "Schedule event " << e.GetMsg() << " in " << timeoutMs << std::endl;

    Queue.push(copy);
}

void TEventQueue::DeliverEvents(TContainerHolder &cholder) {
    size_t now = GetCurrentTimeMs();
    while (!Queue.empty() && Queue.top().DueMs <= now) {
        (void)cholder.DeliverEvent(Queue.top());
        Queue.pop();
    }

    DaemonStat->QueuedEvents = Queue.size();
}

int TEventQueue::GetNextTimeout() {
    if (Queue.empty()) {
        return -1;
    } else {
        size_t now = GetCurrentTimeMs();
        size_t due = Queue.top().DueMs;

        if (now > due)
            return 0;
        else
            return due - now;
    }
}
