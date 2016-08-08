#include "config.hpp"
#include "statistics.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/worker.hpp"

class TEventWorker : public TWorker<TEvent, std::priority_queue<TEvent>> {
    std::shared_ptr<TContainerHolder> Holder;
public:
    TEventWorker(std::shared_ptr<TContainerHolder> holder, const size_t nr) : TWorker("portod-event", nr), Holder(holder) {}

    const TEvent &Top() override {
        return Queue.top();
    }

    void Wait(TScopedLock &lock) override {
        if (!Valid)
            return;

        Statistics->QueuedEvents = Queue.size();

        if (Queue.size()) {
            auto now = GetCurrentTimeMs();
            if (Top().DueMs <= now)
                return;
            auto timeout = Top().DueMs - now;
            Statistics->SlaveTimeoutMs = timeout;
            Cv.wait_for(lock, std::chrono::milliseconds(timeout));
        } else {
            Statistics->SlaveTimeoutMs = 0;
            TWorker::Wait(lock);
        }
    }

    bool Handle(const TEvent &event) override {
        if (event.DueMs <= GetCurrentTimeMs()) {
            (void)Holder->DeliverEvent(event);
            return true;
        }

        return false;
    }
};

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
        case EEventType::WaitTimeout:
            return "wait timeout";
        case EEventType::UpdateNetwork:
            return "update network";
        case EEventType::DestroyWeak:
            return "destroy weak";
        default:
            return "unknown event";
    }
}

bool TEvent::operator<(const TEvent& rhs) const {
    return DueMs >= rhs.DueMs;
}

void TEventQueue::Add(uint64_t timeoutMs, const TEvent &e) {
    TEvent copy = e;
    copy.DueMs = GetCurrentTimeMs() + timeoutMs;

    if (Verbose)
        L() << "Schedule event " << e.GetMsg() << " in " << timeoutMs << " (now " << GetCurrentTimeMs() << " will fire at " << copy.DueMs << ")" << std::endl;

    Worker->Push(copy);
}

TEventQueue::TEventQueue(std::shared_ptr<TContainerHolder> holder) {
    Worker = std::make_shared<TEventWorker>(holder, config().daemon().event_workers());
}

void TEventQueue::Start() {
    Worker->Start();
}

void TEventQueue::Stop() {
    Worker->Stop();
}
