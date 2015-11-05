#include "context.hpp"
#include "epoll.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "subsystem.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "qdisc.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/mount.hpp"

TContext::TContext() {
    Storage = std::make_shared<TKeyValueStorage>(config().keyval().file().path());
    VolumeStorage = std::make_shared<TKeyValueStorage>(config().volumes().keyval().file().path());
    Net = std::make_shared<TNetwork>();
    EpollLoop = std::make_shared<TEpollLoop>();
    Cholder = std::make_shared<TContainerHolder>(EpollLoop, Net, Storage);
    Queue = std::make_shared<TEventQueue>(Cholder);
    Cholder->Queue = Queue;
    Vholder = std::make_shared<TVolumeHolder>(VolumeStorage);
}

TError TContext::CreateDaemonCgs() {
    DaemonCgs[memorySubsystem] = memorySubsystem->GetRootCgroup()->GetChild(PORTO_DAEMON_CGROUP);
    DaemonCgs[cpuacctSubsystem] = cpuacctSubsystem->GetRootCgroup()->GetChild(PORTO_DAEMON_CGROUP);

    for (auto cg : DaemonCgs) {
        TError error = cg.second->Create();
        if (error)
            return error;

        // portod-slave
        error = cg.second->Attach(GetPid());
        if (error)
            return error;

        // portod master
        error = cg.second->Attach(GetPPid());
        if (error)
            return error;
    }

    if (!config().daemon().debug()) {
        TError error = memorySubsystem->SetLimit(DaemonCgs[memorySubsystem], config().daemon().memory_limit());
        if (error)
            return error;
    }

    return TError::Success();
}

TError TContext::Initialize() {
    TError error = EpollLoop->Create();
    if (error)
        return error;

    // don't fail, try to recover anyway
    error = Storage->MountTmpfs(config().keyval().size());
    if (error)
        L_ERR() << "Can't create key-value storage, skipping recovery: " << error << std::endl;
    error = VolumeStorage->MountTmpfs(config().volumes().keyval().size());
    if (error)
        L_ERR() << "Can't create key-value storage, skipping recovery: " << error << std::endl;

    NetEvt = std::make_shared<TNl>();
    error = NetEvt->Connect();
    if (error) {
        L_ERR() << "Can't connect netlink events socket: " << error << std::endl;
        return error;
    }

    error = NetEvt->SubscribeToLinkUpdates();
    if (error) {
        L_ERR() << "Can't subscribe netlink socket to events: " << error << std::endl;
        return error;
    }

    error = Net->Prepare();
    if (error) {
        L_ERR() << "Can't prepare network: " << error << std::endl;
        return error;
    }

    for (auto &link : Net->GetLinks())
        L() << "Using " << link->GetAlias() << " interface" << std::endl;

    auto holder_lock = Cholder->ScopedLock();

    error = Cholder->CreateRoot(holder_lock);
    if (error) {
        L_ERR() << "Can't create root container: " << error << std::endl;
        return error;
    }

    error = Cholder->CreatePortoRoot(holder_lock);
    if (error) {
        L_ERR() << "Can't create porto root container: " << error << std::endl;
        return error;
    }

    error = CreateDaemonCgs();
    if (error)
        return error;

    return TError::Success();
}

TError TContext::Destroy() {
    TError error;

    if (NetEvt)
        NetEvt->Disconnect();

    {
        auto holder_lock = Cholder->ScopedLock();
        Cholder->DestroyRoot(holder_lock);
        Vholder->Destroy();
    }

    error = Storage->Destroy();
    if (error)
        L_ERR() << "Can't destroy key-value storage: " << error << std::endl;

    error = VolumeStorage->Destroy();
    if (error)
        L_ERR() << "Can't destroy volume key-value storage: " << error << std::endl;

    error = Net->Destroy();
    if (error)
        L_ERR() << "Can't destroy network: " << error << std::endl;

    return TError::Success();
}
