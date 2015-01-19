#include "context.hpp"
#include "util/log.hpp"
#include "config.hpp"

TError TContext::Initialize() {
    TError error;

    // don't fail, try to recover anyway
    error = Storage->MountTmpfs();
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

    error = Net->Destroy();
    if (error)
        L_ERR() << "Can't destroy network: " << error << std::endl;

    return TError::Success();
}
