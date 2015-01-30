#include "context.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"
#include "config.hpp"

TContext:: TContext() {
    Storage = std::make_shared<TKeyValueStorage>(TMount("tmpfs", config().keyval().file().path(), "tmpfs", { config().keyval().size() }));
    VolumeStorage = std::make_shared<TKeyValueStorage>(TMount("tmpfs", config().volumes().keyval().file().path(), "tmpfs", { config().volumes().keyval().size() }));
    Queue = std::make_shared<TEventQueue>();
    Net = std::make_shared<TNetwork>();
    EpollLoop = std::make_shared<TEpollLoop>();
    Cholder = std::make_shared<TContainerHolder>(EpollLoop, Queue, Net, Storage);
    Vholder = std::make_shared<TVolumeHolder>(VolumeStorage);
}

TError TContext::Initialize() {
    TError error;

    error = EpollLoop->Create();
    if (error)
        return error;

    // don't fail, try to recover anyway
    error = Storage->MountTmpfs();
    if (error)
        L_ERR() << "Can't create key-value storage, skipping recovery: " << error << std::endl;
    error = VolumeStorage->MountTmpfs();
    if (error)
        L_ERR() << "Can't create key-value storage, skipping recovery: " << error << std::endl;

    if (config().network().enabled()) {
        if (config().network().dynamic_ifaces()) {
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
        }

        TError error = Net->Prepare();
        if (error)
            L_ERR() << "Can't prepare network: " << error << std::endl;


        if (Net->Empty()) {
            L() << "Error: couldn't find suitable network interface" << std::endl;
            return error;
        }

        for (auto &link : Net->GetLinks())
            L() << "Using " << link->GetAlias() << " interface" << std::endl;
    }

    error = Cholder->CreateRoot();
    if (error) {
        L_ERR() << "Can't create root container: " << error << std::endl;
        return error;
    }

    return TError::Success();
}

TError TContext::Destroy() {
    TError error;

    if (NetEvt)
        NetEvt->Disconnect();

    error = Storage->Destroy();
    if (error)
        L_ERR() << "Can't destroy key-value storage: " << error << std::endl;

    error = VolumeStorage->Destroy();
    if (error)
        L_ERR() << "Can't destroy volume key-value storage: " << error << std::endl;

    error = Net->Destroy();
    if (error)
        L_ERR() << "Can't destroy network: " << error << std::endl;

    Cholder->DestroyRoot();
    Vholder->Destroy();

    return TError::Success();
}
