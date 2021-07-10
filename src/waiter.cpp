#include "waiter.hpp"
#include "client.hpp"
#include <time.h>

static std::mutex ContainerWaitersLock;
static std::list<TContainerWaiter *> ContainerWaiters;

static inline std::unique_lock<std::mutex> LockWaiters() {
    return std::unique_lock<std::mutex>(ContainerWaitersLock);
}

TContainerWaiter::~TContainerWaiter() {
    if (Active) {
        auto lock = LockWaiters();
        Deactivate();
    }
}

void TContainerWaiter::Activate(std::shared_ptr<TClient> &client) {
    auto lock = LockWaiters();
    Client = client;
    auto link = Async ? &client->AsyncWaiter : &client->SyncWaiter;
    if (*link) {
        (*link)->Deactivate();
        link->reset();
    }
    if (!Names.empty() || !Wildcards.empty()) {
        *link = shared_from_this();
        Active = true;
        ContainerWaiters.push_back(this);
    }
}

void TContainerWaiter::Deactivate() {
    Active = false;
    ContainerWaiters.remove(this);
}

bool TContainerWaiter::ShouldReport(TContainer &ct) {

    /* Sync wait reports only stopped, dead, respawning, hollow meta */
    if (!Async && ct.State != EContainerState::Stopped &&
            ct.State != EContainerState::Dead &&
            ct.State != EContainerState::Respawning &&
            (ct.State != EContainerState::Meta || ct.RunningChildren))
        return false;

    if (Async && !TargetState.empty() && TargetState != TContainer::StateName(ct.State))
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
            auto client = waiter->Client.lock();

            std::string name;
            if (client && !client->ComposeName(ct.Name, name)) {
                client->MakeReport(name, TContainer::StateName(ct.State), waiter->Async, label, value);

                if (!waiter->Async || !waiter->TargetState.empty()) {
                    ++it;
                    waiter->Deactivate();
                    if (waiter->Async)
                        client->AsyncWaiter.reset();
                    else
                        client->SyncWaiter.reset();
                    continue;
                }
            }
        }
        if (waiter->Active)
            ++it;
        else
            it = ContainerWaiters.erase(it);
    }
}

void TContainerWaiter::Timeout() {
    auto lock = LockWaiters();
    auto client = Client.lock();
    if (client && Active) {
        client->MakeReport("", "timeout", Async);
        Deactivate();
        if (Async)
            client->AsyncWaiter.reset();
        else
            client->SyncWaiter.reset();
    }
}

bool TContainerWaiter::operator==(const TContainerWaiter &waiter) const {
    if (Async != waiter.Async || TargetState != waiter.TargetState)
        return false;

    bool equal = (Names.size() == waiter.Names.size() && std::equal(Names.begin(), Names.end(), waiter.Names.begin()));
    if (!equal)
        return false;

    equal = (Wildcards.size() == waiter.Wildcards.size() && std::equal(Wildcards.begin(), Wildcards.end(), waiter.Wildcards.begin()));
    if (!equal)
        return false;

    return Labels.size() == waiter.Labels.size() && std::equal(Labels.begin(), Labels.end(), waiter.Labels.begin());
}

TError TContainerWaiter::Remove(const TContainerWaiter &waiter, const TClient &client) {
    auto lock = LockWaiters();
    for (auto it = ContainerWaiters.begin(); it != ContainerWaiters.end(); ++it) {
        auto otherWaiter = *it;
        auto waiterClient = otherWaiter->Client.lock();

        // client of waiter is nullptr
        if (&client == waiterClient.get() && waiter == *otherWaiter) {
            otherWaiter->Deactivate();
            if (waiter.Async)
                waiterClient->AsyncWaiter.reset();
            else
                waiterClient->SyncWaiter.reset();
            return OK;
        }
    }
    return TError(EError::InvalidValue, "Waiter not found");
}
