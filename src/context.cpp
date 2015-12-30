#include "context.hpp"
#include "epoll.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/mount.hpp"

TContext::TContext() {
    Storage = std::make_shared<TKeyValueStorage>(config().keyval().file().path());
    VolumeStorage = std::make_shared<TKeyValueStorage>(config().volumes().keyval().file().path());
    EpollLoop = std::make_shared<TEpollLoop>();
    Cholder = std::make_shared<TContainerHolder>(EpollLoop, Storage);
    Queue = std::make_shared<TEventQueue>(Cholder);
    Cholder->Queue = Queue;
    Vholder = std::make_shared<TVolumeHolder>(VolumeStorage);
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

    return TError::Success();
}

TError TContext::Destroy() {
    TError error;

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

    return TError::Success();
}
