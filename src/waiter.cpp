#include "waiter.hpp"
#include "client.hpp"
#include <time.h>

static std::mutex ContainerWaitersLock;
static std::list<TContainerWaiter *> ContainerWaiters;

static inline std::unique_lock<std::mutex> LockWaiters() {
    return std::unique_lock<std::mutex>(ContainerWaitersLock);
}

TContainerWaiter::~TContainerWaiter() {
    PORTO_ASSERT(!Client);
}

void TContainerWaiter::Activate(TClient &client) {
    auto lock = LockWaiters();

    auto link = Async ? &client.AsyncWaiter : &client.SyncWaiter;
    if (*link)
        (*link)->DeactivateLocked();

    PORTO_ASSERT(!Client);

    if (!Names.empty() || !Wildcards.empty()) {
        Client = &client;
        *link = shared_from_this();
        ContainerWaiters.push_back(this);
    }
}

void TContainerWaiter::Deactivate() {
    auto lock = LockWaiters();
    if (Client)
        DeactivateLocked();
}

void TContainerWaiter::DeactivateLocked() {
    PORTO_ASSERT(Client);
    auto link = Async ? &Client->AsyncWaiter : &Client->SyncWaiter;
    PORTO_ASSERT(link->get() == this);

    ContainerWaiters.remove(this);
    Client = nullptr;

    link->reset();
}

bool TContainerWaiter::ShouldReport(TContainer &ct) {

    /* Sync wait reports only stopped, dead, respawning, hollow meta */
    if (!Async &&
        !(ct.State & (EContainerState::STOPPED |
                      EContainerState::DEAD |
                      EContainerState::RESPAWNING)) &&
            (ct.State != EContainerState::META || ct.RunningChildren))
        return false;

    for (auto &nm: Names)
        if (ct.Name == nm)
            return true;

    for (auto &wc: Wildcards)
        if (StringMatch(ct.Name, wc) && ct.Level)
            return true;

    return false;
}

bool TContainerWaiter::ShouldReportLabel(const std::string &label) {
    for (auto &wc: Labels)
        if (StringMatch(label, wc))
            return true;
    return false;
}

void TContainerWaiter::ReportAll(TContainer &ct, const std::string &label, const std::string &value) {
    auto lock = LockWaiters();
    for (auto it = ContainerWaiters.begin(); it != ContainerWaiters.end();) {
        auto waiter = *it;
        if (waiter->ShouldReport(ct) &&
                (label.empty() || (label[0] >= 'a' && label[0] <= 'z') ||
                 waiter->ShouldReportLabel(label))) {

            std::string name;
            if (!waiter->Client->ComposeName(ct.Name, name)) {
                waiter->Client->MakeReport(name, ct.State, waiter->Async, label, value);
                if (!waiter->Async) {
                    ++it;
                    waiter->DeactivateLocked();
                    continue;
                }
            }
        }
        ++it;
    }
}

void TContainerWaiter::Timeout() {
    auto lock = LockWaiters();
    if (Client) {
        Client->MakeReport("", EContainerState::UNDEFINED, Async);
        DeactivateLocked();
    }
}
