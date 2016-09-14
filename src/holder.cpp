#include <algorithm>

#include "statistics.hpp"
#include "holder.hpp"
#include "config.hpp"
#include "container.hpp"
#include "property.hpp"
#include "event.hpp"
#include "client.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "network.hpp"
#include "kvalue.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"

void TContainerHolder::DestroyRoot(TScopedLock &holder_lock) {
    auto list = List(true);

    // we want children to be removed first
    std::reverse(std::begin(list), std::end(list));

    for (auto c: list) {
        TError error = Destroy(holder_lock, c);
        if (error)
            L_ERR() << "Can't destroy container " << c->GetName() << ": " << error << std::endl;
    }
}

TError TContainerHolder::CreateRoot(TScopedLock &holder_lock) {
    TError error;

    error = Create(holder_lock, ROOT_CONTAINER, RootContainer);
    if (error)
        return error;

    if (RootContainer->GetId() != ROOT_CONTAINER_ID)
        return TError(EError::Unknown, "Unexpected root container id");

    error = IdMap.GetAt(DEFAULT_TC_MINOR);
    if (error)
        return error;

    RootContainer->Isolate = false;

    error = RootContainer->Start(true);
    if (error)
        return error;

    return TError::Success();
}

TError TContainerHolder::CreatePortoRoot(TScopedLock &holder_lock) {
    std::shared_ptr<TContainer> container;
    TError error = Create(holder_lock, PORTO_ROOT_CONTAINER, container);
    if (error)
        return error;

    if (container->GetId() != PORTO_ROOT_CONTAINER_ID)
        return TError(EError::Unknown, "Unexpected /porto container id " + std::to_string(container->GetId()));

    container->Isolate = false;

    error = container->Start(true);
    if (error)
        return error;

    ScheduleLogRotatation();

    Statistics->Created = 0;
    Statistics->RestoreFailed = 0;
    Statistics->RemoveDead = 0;
    Statistics->Rotated = 0;
    Statistics->Started = 0;

    return TError::Success();
}

TError TContainerHolder::ValidName(const std::string &name) const {

    if (name.length() == 0)
        return TError(EError::InvalidValue, "container path too short");

    if (name.length() > CONTAINER_PATH_MAX)
        return TError(EError::InvalidValue, "container path too long");

    if (name[0] == '/') {
        if (name == ROOT_CONTAINER || name == PORTO_ROOT_CONTAINER)
            return TError::Success();
        return TError(EError::InvalidValue, "container path starts with '/'");
    }

    for (std::string::size_type first = 0, i = 0; i <= name.length(); i++) {
        switch (name[i]) {
            case '/':
            case '\0':
                if (i == first)
                    return TError(EError::InvalidValue,
                            "double/trailing '/' in container path");
                if (i - first > CONTAINER_NAME_MAX)
                    return TError(EError::InvalidValue,
                            "container name too long: '" +
                            name.substr(first, i - first) + "'");
                if (name.substr(first, i - first) == SELF_CONTAINER)
                    return TError(EError::InvalidValue,
                            "container name 'self' is reserved");
                first = i + 1;
            case 'a'...'z':
            case 'A'...'Z':
            case '0'...'9':
            case '_':
            case '-':
            case '@':
            case ':':
            case '.':
                /* Ok */
                break;
            default:
                return TError(EError::InvalidValue, "forbidden character '" +
                                name.substr(i, 1) + "' in container name");
        }
    }

    return TError::Success();
}

std::shared_ptr<TContainer> TContainerHolder::GetParent(const std::string &name) const {
    std::shared_ptr<TContainer> parent;

    if (name == ROOT_CONTAINER)
        return nullptr;
    else if (name == PORTO_ROOT_CONTAINER)
        return Containers.at(ROOT_CONTAINER);

    std::string::size_type n = name.rfind('/');
    if (n == std::string::npos) {
        return Containers.at(PORTO_ROOT_CONTAINER);
    } else {
        std::string parentName = name.substr(0, n);

        if (Containers.find(parentName) == Containers.end())
            return nullptr;

        return Containers.at(parentName);
    }
}

TError TContainerHolder::Create(TScopedLock &holder_lock, const std::string &name, std::shared_ptr<TContainer> &container) {
    TError error;

    error = ValidName(name);
    if (error)
        return error;

    if (Containers.find(name) != Containers.end())
        return TError(EError::ContainerAlreadyExists, "container " + name + " already exists");

    if (Containers.size() + 1 > config().container().max_total())
        return TError(EError::ResourceNotAvailable, "number of created containers exceeds limit");

    auto parent = GetParent(name);
    if (!parent && name != ROOT_CONTAINER)
        return TError(EError::InvalidValue, "invalid parent container");

    if (parent && parent->GetLevel() == CONTAINER_LEVEL_MAX)
        return TError(EError::InvalidValue, "You shall not go deeper!");

    if (parent) {
        error = CurrentClient->CanControl(*parent, true);
        if (error)
            return error;
    }

    int id;
    error = IdMap.Get(id);
    if (error)
        return error;

    auto c = std::make_shared<TContainer>(shared_from_this(), name, parent, id);
    error = c->Create(CurrentClient->Cred);
    if (error)
        return error;

    Containers[name] = c;
    Statistics->Created++;

    if (parent)
        parent->AddChild(c);

    container = c;

    return TError::Success();
}

TError TContainerHolder::GetLocked(TScopedLock &holder_lock,
                                   const TClient *client,
                                   const std::string &name,
                                   const bool checkPerm,
                                   std::shared_ptr<TContainer> &c,
                                   TNestedScopedLock &l) {
    std::string absoluteName;
    TError error;

    // resolve name
    if (client) {
        error = client->ResolveRelativeName(name, absoluteName, !checkPerm);
        if (error)
            return error;
    } else {
        absoluteName = name;
    }

    // get container
    error = TContainer::Find(absoluteName, c);
    if (error)
        return error;

    // lock container
    l = TNestedScopedLock(*c, holder_lock);

    // make sure it's still alive
    if (!c->IsValid())
        return TError(EError::ContainerDoesNotExist, "container doesn't exist");

    // check permissions
    if (CurrentClient && checkPerm) {
        error = CurrentClient->CanControl(*c);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TContainerHolder::FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &c) {
    TCgroup freezerCg;
    TError error;

    error = FreezerSubsystem.TaskCgroup(pid, freezerCg);
    if (error)
        return error;

    std::string prefix = std::string(PORTO_CGROUP_PREFIX) + "/";
    std::string name = freezerCg.Name;
    std::replace(name.begin(), name.end(), '%', '/');

    auto containers_lock = LockContainers();

    if (!StringStartsWith(name, prefix))
        return TContainer::Find(ROOT_CONTAINER, c);

    return TContainer::Find(name.substr(prefix.length()), c);
}

TError TContainerHolder::Destroy(TScopedLock &holder_lock, std::shared_ptr<TContainer> c) {
    TError error;

    if (c->GetState() != EContainerState::Stopped) {
        error = c->Stop(holder_lock, 0);
        if (error)
            return error;
    }

    Unlink(holder_lock, c);

    return TError::Success();
}

void TContainerHolder::Unlink(TScopedLock &holder_lock, std::shared_ptr<TContainer> c) {
    for (auto name: c->GetChildren()) {
        std::shared_ptr<TContainer> child;
        if (TContainer::Find(name, child))
            continue;

        Unlink(holder_lock, child);
    }

    c->Destroy();

    TError error = IdMap.Put(c->GetId());
    PORTO_ASSERT(!error);
    Containers.erase(c->GetName());
    Statistics->Created--;
}

std::vector<std::shared_ptr<TContainer> > TContainerHolder::List(bool all) const {
    std::vector<std::shared_ptr<TContainer> > ret;

    for (auto c : Containers) {
        PORTO_ASSERT(c.first == c.second->GetName());
        if (!all && c.second->IsPortoRoot())
            continue;
        ret.push_back(c.second);
    }

    return ret;
}

bool TContainerHolder::RestoreFromStorage() {
    std::list<TKeyValue> nodes;

    auto holder_lock = LockContainers();

    TError error = TKeyValue::ListAll(ContainersKV, nodes);
    if (error)
        return false;

    for (auto node = nodes.begin(); node != nodes.end(); ) {
        error = node->Load();
        if (error || !node->Has(P_RAW_NAME)) {
            L_ERR() << "Cannot load " << node->Path << ": " << error << std::endl;
            (void)node->Path.Unlink();
            node = nodes.erase(node);
            continue;
        }
        /* key for sorting */
        node->Name = node->Get(P_RAW_NAME);
        ++node;
    }
    nodes.sort();

    bool restored = false;
    for (auto &node : nodes) {
        restored = true;
        error = Restore(holder_lock, node);
        if (error) {
            L_ERR() << "Can't restore " << node.Name << ": " << error << std::endl;
            Statistics->RestoreFailed++;
            node.Path.Unlink();
            continue;
        }
    }

    return restored;
}

TError TContainerHolder::Restore(TScopedLock &holder_lock, TKeyValue &node) {

    if (node.Name == PORTO_ROOT_CONTAINER && !node.Has(P_CONTROLLERS)) {
        auto ct = Containers[PORTO_ROOT_CONTAINER];
        ct->Controllers = ct->Parent->Controllers;
        config().mutable_container()->set_all_controllers(true);
    }

    if (node.Name == ROOT_CONTAINER || node.Name == PORTO_ROOT_CONTAINER)
        return TError::Success();

    L_ACT() << "Restore container " << node.Name << std::endl;

    auto parent = GetParent(node.Name);
    if (!parent)
        return TError(EError::InvalidValue, "invalid parent container");

    int id = 0;
    TError error = StringToInt(node.Get(P_RAW_ID), id);
    if (!error)
        error = IdMap.Get(id);
    if (error) {
        L_WRN() << "Couldn't restore container id, using " << id << ": " << error << std::endl;
        return error;
    }
    if (!id)
        return TError(EError::Unknown, "Couldn't restore container id");

    auto c = std::make_shared<TContainer>(shared_from_this(), node.Name, parent, id);
    error = c->Restore(holder_lock, node);
    if (error) {
        L_ERR() << "Can't restore container " << node.Name << ": " << error << std::endl;
        return error;
    }

    Containers[node.Name] = c;
    Statistics->Created++;
    return TError::Success();
}

void TContainerHolder::RemoveLeftovers() {
    TError error;

    for (auto hy: Hierarchies) {
        std::vector<TCgroup> cgroups;

        error = hy->RootCgroup().ChildsAll(cgroups);
        if (error)
            L_ERR() << "Cannot dump porto " << hy->Type << " cgroups : "
                    << error << std::endl;

        for (auto cg = cgroups.rbegin(); cg != cgroups.rend(); ++cg) {
            if (!StringStartsWith(cg->Name, PORTO_CGROUP_PREFIX))
                continue;

            if (cg->Name == PORTO_DAEMON_CGROUP &&
                    (hy->Controllers & (CGROUP_MEMORY | CGROUP_CPUACCT)))
                continue;

            bool found = false;
            for (auto &it: Containers) {
                if (it.second->GetCgroup(*hy) == *cg) {
                    found = true;
                    break;
                }
            }
            if (found)
                continue;

            if (!cg->IsEmpty())
                (void)cg->KillAll(9);

            if (hy == &FreezerSubsystem && FreezerSubsystem.IsFrozen(*cg)) {
                (void)FreezerSubsystem.Thaw(*cg);
                if (FreezerSubsystem.IsParentFreezing(*cg))
                    continue;
            }

            (void)cg->Remove();
        }
    }

    for (auto it: Containers) {
        auto container = it.second;
        if (container->IsWeak) {
            auto holder_lock = LockContainers();
            L_ACT() << "Destroy weak container " << it.first << std::endl;
            Destroy(holder_lock, container);
        }
    }
}

void TContainerHolder::ScheduleLogRotatation() {
    TEvent e(EEventType::RotateLogs);
    Queue->Add(config().daemon().rotate_logs_timeout_s() * 1000, e);
}

bool TContainerHolder::DeliverEvent(const TEvent &event) {
    if (Verbose)
        L_EVT() << "Deliver event " << event.GetMsg() << std::endl;

    bool delivered = false;

    auto holder_lock = LockContainers();

    switch (event.Type) {
    case EEventType::OOM:
    {
        std::shared_ptr<TContainer> target = event.Container.lock();
        if (target) {
            // check whether container can die due to OOM under holder lock,
            // assume container state is not changed when only holding
            // container lock
            if (target->MayReceiveOom(event.OOM.Fd)) {
                TNestedScopedLock lock(*target, holder_lock);
                if (target->IsValid() && target->MayReceiveOom(event.OOM.Fd)) {
                    // we don't want any concurrent stop/start/pause/etc and
                    // don't care whether parent acquired or not
                    target->AcquireForced();
                    target->DeliverEvent(holder_lock, event);
                    target->Release();
                    delivered = true;
                }
            }
        }
        break;
    }
    case EEventType::Respawn:
    {
        std::shared_ptr<TContainer> target = event.Container.lock();
        if (target) {
            // check whether container can respawn under holder lock,
            // assume container state is not changed when only holding
            // container lock
            if (target->MayRespawn()) {
                TNestedScopedLock lock(*target, holder_lock);
                if (target->IsValid() && target->MayRespawn() && !target->IsAcquired()) {
                    target->DeliverEvent(holder_lock, event);
                    delivered = true;
                }
            }
        }
        break;
    }
    case EEventType::Exit:
    {
        auto list = List();
        for (auto &target : list) {
            // check whether container can exit under holder lock,
            // assume container state is not changed when only holding
            // container lock
            if (target->WaitTask.Pid == event.Exit.Pid) {
                TNestedScopedLock lock(*target, holder_lock);
                if (target->IsValid() && target->WaitTask.Pid == event.Exit.Pid) {
                    // we don't want any concurrent stop/start/pause/etc and
                    // don't care whether parent acquired or not
                    target->AcquireForced();
                    target->DeliverEvent(holder_lock, event);
                    target->Release();
                    break;
                }
            }
        }
        AckExitStatus(event.Exit.Pid);
        delivered = true;
        break;
    }
    case EEventType::WaitTimeout:
    {
        auto w = event.WaitTimeout.Waiter.lock();
        if (w)
            w->WakeupWaiter(nullptr);
        delivered = true;
        break;
    }
    case EEventType::DestroyWeak:
    {
        auto container = event.Container.lock();
        if (container) {
            TNestedScopedLock lock(*container, holder_lock);
            L_ACT() << "Destroy weak container " << container->GetName() << std::endl;
            Destroy(holder_lock, container);
        }
    }
    case EEventType::RotateLogs:
    {
        { /* gc */
            std::vector<std::string> remove;
            for (auto target : List())
                // don't lock container here, we don't care if we race, we
                // make real check under lock later
                if (target->CanRemoveDead())
                    remove.push_back(target->GetName());

            for (auto name : remove) {
                std::shared_ptr<TContainer> container;
                TError error = TContainer::Find(name, container);
                if (error)
                    continue;

                TNestedScopedLock lock(*container, holder_lock, std::try_to_lock);
                if (!lock.IsLocked() ||
                    !container->IsValid() ||
                    !container->CanRemoveDead())
                    continue;

                TScopedAcquire acquire(container);
                if (!acquire.IsAcquired())
                    continue;

                L_ACT() << "Remove old dead " << name << std::endl;

                error = Destroy(holder_lock, container);
                if (error)
                    L_ERR() << "Can't destroy " << name << ": " << error << std::endl;
                else
                    Statistics->RemoveDead++;
            }
        }

        auto list = List();
        for (auto &target : list) {
            if (target->IsAcquired())
                continue;

            TNestedScopedLock lock(*target, holder_lock);
            if (target->IsValid() && !target->IsAcquired())
                target->DeliverEvent(holder_lock, event);
        }

        ScheduleLogRotatation();
        Statistics->Rotated++;

        holder_lock.unlock();
        TNetwork::RefreshNetworks();

        delivered = true;
        break;
    }
    default:
        L_ERR() << "Unknown event " << event.GetMsg() << std::endl;
    }

    if (!delivered)
        L() << "Couldn't deliver " << event.GetMsg() << std::endl;

    return delivered;
}
