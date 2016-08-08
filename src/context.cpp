#include "context.hpp"
#include "epoll.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "volume.hpp"
#include "client.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/mount.hpp"

TContext::TContext() {
    EpollLoop = std::make_shared<TEpollLoop>();
    Cholder = std::make_shared<TContainerHolder>(EpollLoop);
    Queue = std::make_shared<TEventQueue>(Cholder);
    Cholder->Queue = Queue;
    Vholder = std::make_shared<TVolumeHolder>();
}

TError TContext::Initialize() {
    TError error = EpollLoop->Create();
    if (error)
        return error;

    auto holder_lock = LockContainers();

    SystemClient.StartRequest();

    error = Cholder->CreateRoot(holder_lock);
    if (error) {
        L_ERR() << "Can't create root container: " << error << std::endl;
        goto out;
    }

    error = Cholder->CreatePortoRoot(holder_lock);
    if (error) {
        L_ERR() << "Can't create porto root container: " << error << std::endl;
        goto out;
    }

out:
    SystemClient.FinishRequest();
    return error;
}

TError TContext::Destroy() {
    auto holder_lock = LockContainers();
    Cholder->DestroyRoot(holder_lock);
    Vholder->Destroy();

    return TError::Success();
}
