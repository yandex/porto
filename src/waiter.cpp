#include "waiter.hpp"
#include "client.hpp"
#include <time.h>

static std::mutex ContainerWaitersLock;
static std::list<TContainerWaiter *> ContainerWaiters;

TContainerWaiter::~TContainerWaiter() {
    if (Active) {
        ContainerWaitersLock.lock();
        Deactivate();
        ContainerWaitersLock.unlock();
    }
}

void TContainerWaiter::Activate(std::shared_ptr<TClient> &client) {
    ContainerWaitersLock.lock();
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
    ContainerWaitersLock.unlock();
}

void TContainerWaiter::Deactivate() {
    Active = false;
    ContainerWaiters.remove(this);
}

bool TContainerWaiter::ShouldReport(TContainer &ct) {

    /* Sync wait reports only stopped, dead, hollow meta */
    if (!Async && ct.State != EContainerState::Stopped &&
            ct.State != EContainerState::Dead &&
            (ct.State != EContainerState::Meta || ct.RunningChildren))
        return false;

    for (auto &nm: Names)
        if (ct.Name == nm)
            return true;

    for (auto &wc: Wildcards)
        if (StringMatch(ct.Name, wc))
            return true;

    return false;
}

void TContainerWaiter::ReportAll(TContainer &ct) {
    ContainerWaitersLock.lock();
    for (auto it = ContainerWaiters.begin(); it != ContainerWaiters.end();) {
        auto waiter = *it;
        if (waiter->ShouldReport(ct)) {
            auto client = waiter->Client.lock();

            std::string name;
            if (client && !client->ComposeName(ct.Name, name)) {
                client->MakeReport(name, TContainer::StateName(ct.State), waiter->Async);
                if (!waiter->Async) {
                    waiter->Active = false;
                    client->SyncWaiter.reset();
                }
            }
        }
        if (waiter->Active)
            ++it;
        else
            it = ContainerWaiters.erase(it);
    }
    ContainerWaitersLock.unlock();
}

void TContainerWaiter::Timeout() {
    ContainerWaitersLock.lock();
    auto client = Client.lock();
    if (client) {
        client->MakeReport("", "timeout", Async);
        Deactivate();
        if (Async)
            client->AsyncWaiter.reset();
        else
            client->SyncWaiter.reset();
    }
    ContainerWaitersLock.unlock();
}
