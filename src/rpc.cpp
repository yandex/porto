#include <algorithm>

#include "rpc.hpp"
#include "config.hpp"
#include "version.hpp"
#include "holder.hpp"
#include "property.hpp"
#include "data.hpp"
#include "container_value.hpp"
#include "volume.hpp"
#include "event.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"

using std::string;

static std::string RequestAsString(const rpc::TContainerRequest &req) {
    if (config().log().verbose())
        return req.ShortDebugString();

    if (req.has_create())
        return "create " + req.create().name();
    else if (req.has_destroy())
        return "destroy " + req.destroy().name();
    else if (req.has_list())
        return "list containers";
    else if (req.has_getproperty())
        return "pget "  + req.getproperty().name() + " " + req.getproperty().property();
    else if (req.has_setproperty())
        return "pset " + req.setproperty().name() + " " +
                         req.setproperty().property() + " " +
                         req.setproperty().value();
    else if (req.has_getdata())
        return "dget " + req.getdata().name() + " " + req.getdata().data();
    else if (req.has_get()) {
        std::string ret = "get";

        for (int i = 0; i < req.get().name_size(); i++)
            ret += " " + req.get().name(i);

        if (req.get().name_size() && req.get().variable_size())
            ret += ",";

        for (int i = 0; i < req.get().variable_size(); i++)
            ret += " " + req.get().variable(i);

        return ret;
    } else if (req.has_start())
        return "start " + req.start().name();
    else if (req.has_stop())
        return "stop " + req.stop().name();
    else if (req.has_pause())
        return "pause " + req.pause().name();
    else if (req.has_resume())
        return "resume " + req.resume().name();
    else if (req.has_propertylist())
        return "list available properties";
    else if (req.has_datalist())
        return "list available data";
    else if (req.has_kill())
        return "kill " + req.kill().name() + " " + std::to_string(req.kill().sig());
    else if (req.has_version())
        return "get version";
    else if (req.has_wait()) {
        std::string ret = "wait";

        for (int i = 0; i < req.wait().name_size(); i++)
            ret += " " + req.wait().name(i);

        if (req.wait().has_timeout())
            ret += " timeout " + std::to_string(req.wait().timeout());

        return ret;
    } else if (req.has_createvolume()) {
        std::string ret = "volumeAPI: create " + req.createvolume().path();
        for (auto p: req.createvolume().properties())
            ret += " " + p.name() + "=" + p.value();
        return ret;
    } else if (req.has_linkvolume())
        return "volumeAPI: link " + req.linkvolume().path() + " to " +
                                    req.linkvolume().container();
    else if (req.has_unlinkvolume())
        return "volumeAPI: unlink " + req.unlinkvolume().path() + " from " +
                                      req.unlinkvolume().container();
    else if (req.has_listvolumes())
        return "volumeAPI: list volumes";
    else
        return req.ShortDebugString();
}

static std::string ResponseAsString(const rpc::TContainerResponse &resp) {
    if (config().log().verbose())
        return resp.ShortDebugString();

    switch (resp.error()) {
    case EError::Success:
    {
        std::string ret;

        if (resp.has_list()) {
            for (int i = 0; i < resp.list().name_size(); i++)
                ret += resp.list().name(i) + " ";
        } else if (resp.has_propertylist()) {
            for (int i = 0; i < resp.propertylist().list_size(); i++)
                ret += resp.propertylist().list(i).name()
                    + " (" + resp.propertylist().list(i).desc() + ")";
        } else if (resp.has_datalist()) {
            for (int i = 0; i < resp.datalist().list_size(); i++)
                ret += resp.datalist().list(i).name()
                    + " (" + resp.datalist().list(i).desc() + ")";
        } else if (resp.has_volumelist()) {
            for (auto v: resp.volumelist().volumes())
                ret += v.path() + " ";
        } else if (resp.has_getproperty()) {
            ret = resp.getproperty().value();
        } else if (resp.has_getdata()) {
            ret = resp.getdata().value();
        } else if (resp.has_get()) {
            for (int i = 0; i < resp.get().list_size(); i++) {
                auto entry = resp.get().list(i);

                if (ret.length())
                    ret += "; ";
                ret += entry.name() + ":";

                for (int j = 0; j < entry.keyval_size(); j++)
                    if (entry.keyval(j).has_error())
                        ret += " " + entry.keyval(j).variable() + "=" + std::to_string(entry.keyval(j).error()) + "?";
                    else if (entry.keyval(j).has_value())
                        ret += " " + entry.keyval(j).variable() + "=" + entry.keyval(j).value();
            }
        } else if (resp.has_version())
            ret = resp.version().tag() + " #" + resp.version().revision();
        else if (resp.has_wait())
            ret = resp.wait().name() + " isn't running";
        else
            ret = "Ok";
        return ret;
        break;
    }
    case EError::Unknown:
        return "Error: Unknown (" + resp.errormsg() + ")";
        break;
    case EError::InvalidMethod:
        return "Error: InvalidMethod (" + resp.errormsg() + ")";
        break;
    case EError::ContainerAlreadyExists:
        return "Error: ContainerAlreadyExists (" + resp.errormsg() + ")";
        break;
    case EError::ContainerDoesNotExist:
        return "Error: ContainerDoesNotExist (" + resp.errormsg() + ")";
        break;
    case EError::InvalidProperty:
        return "Error: InvalidProperty (" + resp.errormsg() + ")";
        break;
    case EError::InvalidData:
        return "Error: InvalidData (" + resp.errormsg() + ")";
        break;
    case EError::InvalidValue:
        return "Error: InvalidValue (" + resp.errormsg() + ")";
        break;
    case EError::InvalidState:
        return "Error: InvalidState (" + resp.errormsg() + ")";
        break;
    case EError::NotSupported:
        return "Error: NotSupported (" + resp.errormsg() + ")";
        break;
    case EError::ResourceNotAvailable:
        return "Error: ResourceNotAvailable (" + resp.errormsg() + ")";
        break;
    case EError::Permission:
        return "Error: Permission (" + resp.errormsg() + ")";
        break;
    case EError::VolumeAlreadyExists:
        return "Error: VolumeAlreadyExists (" + resp.errormsg() + ")";
        break;
    case EError::VolumeNotFound:
        return "Error: VolumeNotFound (" + resp.errormsg() + ")";
        break;
    case EError::VolumeAlreadyLinked:
        return "Error: VolumeAlreadyLinked (" + resp.errormsg() + ")";
        break;
    case EError::VolumeNotLinked:
        return "Error: VolumeNotLinked (" + resp.errormsg() + ")";
        break;
    case EError::Busy:
        return "Error: Busy (" + resp.errormsg() + ")";
        break;
    case EError::LayerAlreadyExists:
        return "Error: LayerAlreadyExists (" + resp.errormsg() + ")";
    case EError::LayerNotFound:
        return "Error: LayerNotFound (" + resp.errormsg() + ")";
    case EError::NoSpace:
        return "Error: NoSpace (" + resp.errormsg() + ")";
        break;
    default:
        return resp.ShortDebugString();
        break;
    };
}

static bool InfoRequest(const rpc::TContainerRequest &req) {
    return
        req.has_list() ||
        req.has_getproperty() ||
        req.has_getdata() ||
        req.has_get() ||
        req.has_propertylist() ||
        req.has_datalist() ||
        req.has_version() ||
        req.has_wait() ||
        req.has_listvolumeproperties() ||
        req.has_listvolumes() ||
        req.has_listlayers();
}

static bool ValidRequest(const rpc::TContainerRequest &req) {
    return
        req.has_create() +
        req.has_destroy() +
        req.has_list() +
        req.has_getproperty() +
        req.has_setproperty() +
        req.has_getdata() +
        req.has_get() +
        req.has_start() +
        req.has_stop() +
        req.has_pause() +
        req.has_resume() +
        req.has_propertylist() +
        req.has_datalist() +
        req.has_kill() +
        req.has_version() +
        req.has_wait() +
        req.has_listvolumeproperties() +
        req.has_createvolume() +
        req.has_linkvolume() +
        req.has_unlinkvolume() +
        req.has_listvolumes() +
        req.has_importlayer() +
        req.has_exportlayer() +
        req.has_removelayer() +
        req.has_listlayers() == 1;
}

static void SendReply(std::shared_ptr<TClient> client,
                      rpc::TContainerResponse &response,
                      bool log) {
    if (!client) {
        std::cout << "no client" << std::endl;
        return;
    }

    google::protobuf::io::FileOutputStream post(client->GetFd());

    if (response.IsInitialized()) {
        if (WriteDelimitedTo(response, &post)) {
            post.Flush();

            if (log)
                L_RSP() << ResponseAsString(response) << " to " << *client
                        << " (request took " << client->GetRequestTime() << "ms)"
                        << std::endl;
        } else {
            L_RSP() << "Protobuf write error for " << client->GetFd() << " " << strerror(errno) << std:: endl;
        }
    }

    auto loop = client->EpollLoop.lock();
    if (loop) {
        TError error = loop->EnableSource(client);
        if (error)
            L_WRN() << "Can't enable client " << client->GetFd() << ": " << error << std::endl;
    }
}

static TError CheckRequestPermissions(std::shared_ptr<TClient> client) {
    if (client->Readonly())
        return TError(EError::Permission, "Client is not a member of porto group");

    return TError::Success();
}

noinline TError CreateContainer(TContext &context,
                                const rpc::TContainerCreateRequest &req,
                                rpc::TContainerResponse &rsp,
                                std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> clientContainer;
    err = client->GetContainer(clientContainer);
    if (err)
        return err;

    std::string name;
    err = clientContainer->AbsoluteName(req.name(), name);
    if (err)
        return err;

    auto parent = context.Cholder->GetParent(name);
    if (!parent)
        return TError(EError::InvalidValue, "invalid parent container");

    TNestedScopedLock lock(*parent, holder_lock);
    if (!parent->IsValid())
        return TError(EError::ContainerDoesNotExist, "Parent container doesn't exist");

    if (parent->IsAcquired())
        return TError(EError::Busy, "Parent container " + parent->GetName() + " is busy");

    std::shared_ptr<TContainer> container;
    err = context.Cholder->Create(holder_lock, name, client->GetCred(), container);
    if (!err) {
        TNestedScopedLock lock(*container, holder_lock);
        if (container->IsValid())
            container->Journal("created", client);
    }

    return err;
}

noinline TError DestroyContainer(TContext &context,
                                 const rpc::TContainerDestroyRequest &req,
                                 rpc::TContainerResponse &rsp,
                                 std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> parent;

    // we don't want to hold container shared_ptr because Destroy
    // might think that it has some parent that holds it
    {
        std::shared_ptr<TContainer> container;
        TNestedScopedLock lock;
        err = context.Cholder->GetLocked(holder_lock, client, req.name(), true, container, lock);
        if (err)
            return err;

        parent = container->GetParent();

        {
            TScopedAcquire acquire(container);
            if (!acquire.IsAcquired())
                return TError(EError::Busy, "Can't destroy busy container");

            err = context.Cholder->Destroy(holder_lock, container);
            if (err)
                return err;
        }

        container->Journal("destroyed", client);
    }

    if (parent) {
        TNestedScopedLock lock(*parent, holder_lock);
        parent->CleanupExpiredChildren();
    }

    return TError::Success();
}

noinline TError StartContainer(TContext &context,
                               const rpc::TContainerStartRequest &req,
                               rpc::TContainerResponse &rsp,
                               std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> clientContainer;
    err = client->GetContainer(clientContainer);
    if (err)
        return err;

    /* Check if target container exists */
    std::string name;
    err = clientContainer->AbsoluteName(req.name(), name);
    if (err)
        return err;

    std::shared_ptr<TContainer> target;
    err = context.Cholder->Get(name, target);
    if (err)
        return err;

    std::vector<std::string> nameVec;
    err = SplitString(name, '/', nameVec);
    if (err)
        return TError(EError::InvalidValue, "Invalid container name " + req.name());

    std::shared_ptr<TContainer> topContainer = nullptr;

    name = "";
    for (auto i = nameVec.begin(); i != nameVec.end(); i++) {
        if (!name.empty())
            name += "/";
        name += *i;

        std::shared_ptr<TContainer> container;
        TNestedScopedLock lock;
        err = context.Cholder->GetLocked(holder_lock, nullptr, name, false, container, lock);
        if (err)
            goto release;

        err = container->CheckPermission(client->GetCred());
        if (err)
            goto release;

        if (i + 1 != nameVec.end())
            if (container->GetState() == EContainerState::Running ||
                container->GetState() == EContainerState::Meta)
                continue;

        if (!topContainer) {
            topContainer = container;

            if (!topContainer->Acquire())
                return TError(EError::Busy, "Can't start busy container " + topContainer->GetName());
        }

        std::string cmd = container->Prop->Get<std::string>(P_COMMAND);
        bool meta = i + 1 != nameVec.end() && cmd.empty();

        auto parent = container->GetParent();
        if (parent) {
            // we got concurrent request which stopped our parent
            if (parent->GetState() != EContainerState::Running &&
                parent->GetState() != EContainerState::Meta) {
                err = TError(EError::Busy, "Can't start busy container (concurrent stop/destroy)");
                goto release;
            }
        }

        err = container->Start(client, meta);

        if (err)
            goto release;

        if (topContainer == container)
            container->Journal("started", client);
        else
            container->Journal("started", target);
    }

release:
    if (topContainer)
        topContainer->Release();

    return err;
}

noinline TError StopContainer(TContext &context,
                              const rpc::TContainerStopRequest &req,
                              rpc::TContainerResponse &rsp,
                              std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, client, req.name(), true, container, lock);
    if (error)
        return error;

    TScopedAcquire acquire(container);
    if (!acquire.IsAcquired())
        return TError(EError::Busy, "Can't stop busy container");

    err = container->StopTree(holder_lock);
    if (!err)
        container->Journal("stopped", client);

    return err;
}

noinline TError PauseContainer(TContext &context,
                               const rpc::TContainerPauseRequest &req,
                               rpc::TContainerResponse &rsp,
                               std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, client, req.name(), true, container, lock);
    if (error)
        return error;

    TScopedAcquire acquire(container);
    if (!acquire.IsAcquired())
        return TError(EError::Busy, "Can't pause busy container");

    err = container->Pause(holder_lock);
    if (!err)
        container->Journal("paused", client);

    return err;
}

noinline TError ResumeContainer(TContext &context,
                                const rpc::TContainerResumeRequest &req,
                                rpc::TContainerResponse &rsp,
                                std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, client, req.name(), true, container, lock);
    if (error)
        return error;

    TScopedAcquire acquire(container);
    if (!acquire.IsAcquired())
        return TError(EError::Busy, "Can't resume busy container");

    err = container->Resume(holder_lock);
    if (!err)
        container->Journal("resumed", client);

    return err;
}

noinline TError ListContainers(TContext &context,
                               rpc::TContainerResponse &rsp,
                               std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    for (auto &c : context.Cholder->List()) {
        std::shared_ptr<TContainer> clientContainer;
        TError err = client->GetContainer(clientContainer);
        if (err)
            return err;

        std::string name;
        err = clientContainer->RelativeName(*c, name);
        if (!err)
            rsp.mutable_list()->add_name(name);
    }

    return TError::Success();
}

noinline TError GetContainerProperty(TContext &context,
                                     const rpc::TContainerGetPropertyRequest &req,
                                     rpc::TContainerResponse &rsp,
                                     std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    std::shared_ptr<TContainer> clientContainer;
    TError err = client->GetContainer(clientContainer);
    if (err)
        return err;

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, client, req.name(), false, container, lock);
    if (error)
        return error;

    if (!container->IsValid())
        return TError(EError::ContainerDoesNotExist, "container doesn't exist");

    string value;
    err = container->GetProperty(req.property(), value, client);
    if (!err)
        rsp.mutable_getproperty()->set_value(value);

    return err;
}

noinline TError SetContainerProperty(TContext &context,
                                     const rpc::TContainerSetPropertyRequest &req,
                                     rpc::TContainerResponse &rsp,
                                     std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, client, req.name(), true, container, lock);
    if (error)
        return error;

    TScopedAcquire acquire(container);
    if (!acquire.IsAcquired())
        return TError(EError::Busy, "Can't set property " + req.property() + " of busy container " + container->GetName());

    err = container->SetProperty(req.property(), req.value(), client);
    if (!err)
        container->Journal("set " + req.property() + " to " + req.value(), client);

    return err;
}

noinline TError GetContainerData(TContext &context,
                                 const rpc::TContainerGetDataRequest &req,
                                 rpc::TContainerResponse &rsp,
                                 std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    std::shared_ptr<TContainer> clientContainer;
    TError err = client->GetContainer(clientContainer);
    if (err)
        return err;

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, client, req.name(), false, container, lock);
    if (error)
        return error;

    if (!container->IsValid())
        return TError(EError::ContainerDoesNotExist, "container doesn't exist");

    string value;
    err = container->GetData(req.data(), value, client);
    if (!err)
        rsp.mutable_getdata()->set_value(value);

    return err;
}

noinline TError GetContainerCombined(TContext &context,
                                     const rpc::TContainerGetRequest &req,
                                     rpc::TContainerResponse &rsp,
                                     std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    if (!req.variable_size())
        return TError(EError::InvalidValue, "Properties/data are not specified");

    if (!req.name_size())
        return TError(EError::InvalidValue, "Containers are not specified");

    std::shared_ptr<TContainer> clientContainer;
    TError err = client->GetContainer(clientContainer);
    if (err)
        return err;

    auto get = rsp.mutable_get();

    for (int i = 0; i < req.name_size(); i++) {
        auto relname = req.name(i);

        auto entry = get->add_list();
        entry->set_name(relname);

        std::string name;
        std::shared_ptr<TContainer> container;

        TNestedScopedLock lock;
        TError containerError = clientContainer->AbsoluteName(relname, name, true);
        if (!containerError) {
            containerError = context.Cholder->Get(name, container);
            if (!containerError && container) {
                if (container->IsAcquired()) {
                    containerError = TError(EError::Busy, "Can't get data and property of busy container");
                } else {
                    lock = TNestedScopedLock(*container, holder_lock);
                    if (!container->IsValid())
                        containerError = TError(EError::ContainerDoesNotExist, "container doesn't exist");
                    else if (container->IsAcquired())
                        containerError = TError(EError::Busy, "Can't get data and property of busy container");
                }
            }
        }

        for (int j = 0; j < req.variable_size(); j++) {
            auto var = req.variable(j);

            auto keyval = entry->add_keyval();
            std::string value;

            TError error = containerError;
            if (!error && container) {
                std::string name = var, idx;
                TContainer::ParsePropertyName(name, idx);

                if (container->Prop->IsValid(name))
                    error = container->GetProperty(var, value, client);
                else if (container->Data->IsValid(name))
                    error = container->GetData(var, value, client);
                else
                    error = TError(EError::InvalidValue, "Unknown property or data " + var);
            }

            keyval->set_variable(var);
            if (error) {
                keyval->set_error(error.GetError());
                keyval->set_errormsg(error.GetMsg());
            } else {
                keyval->set_value(value);
            }
        }
    }

    return TError::Success();
}

noinline TError ListProperty(TContext &context,
                             rpc::TContainerResponse &rsp) {
    auto holder_lock = context.Cholder->ScopedLock();

    auto list = rsp.mutable_propertylist();

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, nullptr, ROOT_CONTAINER, false, container, lock);
    if (error)
        return TError(EError::Unknown, "Can't find root container");

    for (auto name : container->Prop->List()) {
        auto av = container->Prop->Find(name);
        if (av->GetFlags() & HIDDEN_VALUE)
            continue;

        auto cv = ToContainerValue(av);
        if (!cv->IsImplemented())
            continue;
        auto entry = list->add_list();

        entry->set_name(name);
        entry->set_desc(cv->GetDesc());
    }

    return TError::Success();
}

noinline TError ListData(TContext &context,
                         rpc::TContainerResponse &rsp) {
    auto holder_lock = context.Cholder->ScopedLock();

    auto list = rsp.mutable_datalist();

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, nullptr, ROOT_CONTAINER, false, container, lock);
    if (error)
        return TError(EError::Unknown, "Can't find root container");

    for (auto name : container->Data->List()) {
        auto av = container->Data->Find(name);
        if (av->GetFlags() & HIDDEN_VALUE)
            continue;

        auto cv = ToContainerValue(av);
        if (!cv->IsImplemented())
            continue;
        auto entry = list->add_list();

        entry->set_name(name);
        entry->set_desc(cv->GetDesc());
    }

    return TError::Success();
}

noinline TError Kill(TContext &context,
                     const rpc::TContainerKillRequest &req,
                     rpc::TContainerResponse &rsp,
                     std::shared_ptr<TClient> client) {
    auto holder_lock = context.Cholder->ScopedLock();

    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::shared_ptr<TContainer> container;
    TNestedScopedLock lock;
    TError error = context.Cholder->GetLocked(holder_lock, client, req.name(), true, container, lock);
    if (error)
        return error;

    TScopedAcquire acquire(container);
    if (!acquire.IsAcquired())
        return TError(EError::Busy, "Can't kill busy container");

    err = container->Kill(req.sig());
    if (!err)
        container->Journal("killed with " + req.sig(), client);
    return err;
}

noinline TError Version(TContext &context,
                        rpc::TContainerResponse &rsp) {
    auto ver = rsp.mutable_version();

    ver->set_tag(GIT_TAG);
    ver->set_revision(GIT_REVISION);

    return TError::Success();
}

noinline TError Wait(TContext &context,
                     const rpc::TContainerWaitRequest &req,
                     rpc::TContainerResponse &rsp,
                     std::shared_ptr<TClient> client) {
    auto lock = context.Cholder->ScopedLock();

    if (!req.name_size())
        return TError(EError::InvalidValue, "Containers are not specified");

    std::shared_ptr<TContainer> clientContainer;
    TError err = client->GetContainer(clientContainer);
    if (err)
        return err;

    auto fn = [] (std::shared_ptr<TClient> client,
                  TError error, std::string name) {
        rpc::TContainerResponse response;
        response.set_error(error.GetError());
        response.mutable_wait()->set_name(name);
        SendReply(client, response, true);
    };

    auto waiter = std::make_shared<TContainerWaiter>(client, fn);

    for (int i = 0; i < req.name_size(); i++) {
        std::string name = req.name(i);
        std::string abs_name;
        err = clientContainer->AbsoluteName(name, abs_name);
        if (err) {
            rsp.mutable_wait()->set_name(name);
            return err;
        }

        std::shared_ptr<TContainer> container;
        err = context.Cholder->Get(abs_name, container);
        if (err) {
            rsp.mutable_wait()->set_name(name);
            return err;
        }

        container->AddWaiter(waiter);
    }

    client->Waiter = waiter;

    if (req.has_timeout()) {
        TEvent e(EEventType::WaitTimeout, nullptr);
        e.WaitTimeout.Waiter = waiter;
        context.Queue->Add(req.timeout(), e);
    }

    return TError::Queued();
}

noinline TError ListVolumeProperties(TContext &context,
                                     const rpc::TVolumePropertyListRequest &req,
                                     rpc::TContainerResponse &rsp,
                                     std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    auto list = rsp.mutable_volumepropertylist();
    for (auto kv: context.Vholder->ListProperties()) {
        auto p = list->add_properties();
        p->set_name(kv.first);
        p->set_desc(kv.second);
    }

    return TError::Success();
}

noinline void FillVolumeDescription(rpc::TVolumeDescription *desc,
                                    TPath container_root, TPath volume_path,
                                    std::shared_ptr<TVolume> volume) {
    desc->set_path(volume_path.ToString());
    for (auto kv: volume->GetProperties(container_root)) {
        auto p = desc->add_properties();
        p->set_name(kv.first);
        p->set_value(kv.second);
    }
    for (auto name: volume->GetContainers())
        desc->add_containers(name);
}

noinline TError CreateVolume(TContext &context,
                             const rpc::TVolumeCreateRequest &req,
                             rpc::TContainerResponse &rsp,
                             std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TContainer> clientContainer;
    error = client->GetContainer(clientContainer);
    if (error)
        return error;

    std::map<std::string, std::string> properties;
    for (auto p: req.properties())
        properties[p.name()] = p.value();

    auto vholder_lock = context.Vholder->ScopedLock();
    std::shared_ptr<TVolume> volume;
    error = context.Vholder->Create(volume);
    if (error)
        return error;

    /* cannot block: volume is not registered yet */
    auto volume_lock = volume->ScopedLock();

    auto container_root = clientContainer->RootPath();

    TPath volume_path("");
    if (req.has_path() && !req.path().empty())
        volume_path = container_root / req.path();

    error = volume->Configure(volume_path, client->GetCred(),
                              clientContainer, properties, *context.Vholder);
    if (error) {
        context.Vholder->Remove(volume);
        return error;
    }

    volume_path = container_root.InnerPath(volume->GetPath(), true);
    if (volume_path.IsEmpty()) {
        /* sanity check */
        error = TError(EError::Unknown, "volume inner path not found");
        L_ERR() << error << " " << volume->GetPath() << " in " << container_root << std::endl;
        context.Vholder->Remove(volume);
        return error;
    }

    error = context.Vholder->Register(volume);
    if (error) {
        context.Vholder->Remove(volume);
        return error;
    }

    vholder_lock.unlock();

    error = volume->Build();

    vholder_lock.lock();

    if (error) {
        L_WRN() << "Can't build volume: " << error << std::endl;
        context.Vholder->Unregister(volume);
        context.Vholder->Remove(volume);
        return error;
    }

    auto cholder_lock = context.Cholder->ScopedLock();

    error = volume->LinkContainer(clientContainer->GetName());
    if (error) {
        cholder_lock.unlock();

        L_WRN() << "Can't link volume" << std::endl;
        (void)volume->Destroy();
        context.Vholder->Unregister(volume);
        context.Vholder->Remove(volume);
        return error;
    }
    clientContainer->LinkVolume(context.Vholder, volume);
    cholder_lock.unlock();

    volume->SetReady(true);
    vholder_lock.unlock();

    FillVolumeDescription(rsp.mutable_volume(), container_root, volume_path, volume);
    volume_lock.unlock();

    return TError::Success();
}

noinline TError LinkVolume(TContext &context,
                           const rpc::TVolumeLinkRequest &req,
                           rpc::TContainerResponse &rsp,
                           std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TContainer> clientContainer;
    error = client->GetContainer(clientContainer);
    if (error)
        return error;

    auto cholder_lock = context.Cholder->ScopedLock();
    std::shared_ptr<TContainer> container;
    if (req.has_container()) {
        std::string name;
        TError err = clientContainer->AbsoluteName(req.container(), name, true);
        if (err)
            return err;

        TError error = context.Cholder->Get(name, container);
        if (error)
            return error;

        error = container->CheckPermission(client->GetCred());
        if (error)
            return error;
    } else {
        container = clientContainer;
    }
    cholder_lock.unlock();

    TPath volume_path = clientContainer->RootPath() / req.path();

    auto vholder_lock = context.Vholder->ScopedLock();
    auto volume = context.Vholder->Find(volume_path);

    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");
    vholder_lock.unlock();

    auto volume_lock = volume->ScopedLock();
    if (!volume->IsReady())
        return TError(EError::Busy, "Volume not ready");

    error = volume->CheckPermission(client->GetCred());
    if (error)
        return error;

    vholder_lock.lock();
    if (!container->LinkVolume(context.Vholder, volume))
        return TError(EError::VolumeAlreadyLinked, "Already linked");

    return volume->LinkContainer(container->GetName());
}

noinline TError UnlinkVolume(TContext &context,
                             const rpc::TVolumeUnlinkRequest &req,
                             rpc::TContainerResponse &rsp,
                             std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TContainer> clientContainer;
    error = client->GetContainer(clientContainer);
    if (error)
        return error;

    auto vholder_lock = context.Vholder->ScopedLock();
    auto cholder_lock = context.Cholder->ScopedLock();

    std::shared_ptr<TContainer> container;
    if (req.has_container()) {
        std::string name;
        TError err = clientContainer->AbsoluteName(req.container(), name, true);
        if (err)
            return err;

        TError error = context.Cholder->Get(name, container);
        if (error)
            return error;

        error = container->CheckPermission(client->GetCred());
        if (error)
            return error;
    } else {
        container = clientContainer;
    }

    TPath volume_path = clientContainer->RootPath() / req.path();

    std::shared_ptr<TVolume> volume = context.Vholder->Find(volume_path);
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");

    error = volume->CheckPermission(client->GetCred());
    if (error)
        return error;

    if (!container->UnlinkVolume(volume))
        return TError(EError::VolumeNotLinked, "Container not linked to the volume");

    if (!volume->UnlinkContainer(container->GetName()))
        return TError::Success(); /* Still linked to somebody */

    cholder_lock.unlock();
    vholder_lock.unlock();

    auto volume_lock = volume->ScopedLock();
    if (!volume->IsReady())
        return TError::Success();

    vholder_lock.lock();
    error = volume->SetReady(false);
    if (error)
        return error;

    vholder_lock.unlock();
    error = volume->Destroy();

    vholder_lock.lock();
    context.Vholder->Unregister(volume);
    context.Vholder->Remove(volume);
    vholder_lock.unlock();

    volume_lock.unlock();

    return error;
}

noinline TError ListVolumes(TContext &context,
                            const rpc::TVolumeListRequest &req,
                            rpc::TContainerResponse &rsp,
                            std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    std::shared_ptr<TContainer> clientContainer;
    TError error = client->GetContainer(clientContainer);
    if (error)
        return error;

    TPath container_root = clientContainer->RootPath();

    auto vholder_lock = context.Vholder->ScopedLock();

    if (req.has_path() && !req.path().empty()) {
        TPath volume_path = container_root / req.path();
        auto volume = context.Vholder->Find(volume_path);
        if (volume == nullptr)
            return TError(EError::VolumeNotFound, "volume not found");
        auto desc = rsp.mutable_volumelist()->add_volumes();
        volume_path = container_root.InnerPath(volume->GetPath(), true);
        FillVolumeDescription(desc, container_root, volume_path, volume);
        return TError::Success();
    }

    for (auto path : context.Vholder->ListPaths()) {
        auto volume = context.Vholder->Find(path);
        if (volume == nullptr)
            continue;

        auto containers = volume->GetContainers();
        if (req.has_container() &&
            std::find(containers.begin(), containers.end(),
                      req.container()) == containers.end())
            continue;

        TPath volume_path = container_root.InnerPath(volume->GetPath(), true);
        if (volume_path.IsEmpty())
            continue;

        auto desc = rsp.mutable_volumelist()->add_volumes();
        FillVolumeDescription(desc, container_root, volume_path, volume);
    }

    return TError::Success();
}

static bool LayerInUse(TContext &context, TPath layer) {
    for (auto path : context.Vholder->ListPaths()) {
        auto volume = context.Vholder->Find(path);
        if (volume == nullptr)
            continue;
        for (auto l: volume->GetLayers()) {
            if (l.NormalPath() == layer)
                return true;
        }
    }
    return false;
}

noinline TError ImportLayer(TContext &context,
                            const rpc::TLayerImportRequest &req,
                            std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TContainer> clientContainer;
    error = client->GetContainer(clientContainer);
    if (error)
        return error;

    std::string layer_name = req.layer();
    if (layer_name.find_first_of("/\\\n\r\t ") != string::npos ||
        layer_name == "_tmp_")
        return TError(EError::InvalidValue, "invalid layer name");

    TPath layers = TPath(config().volumes().layers_dir());
    TPath layers_tmp = layers / "_tmp_";
    TPath layer = layers / layer_name;
    TPath layer_tmp = layers_tmp / layer_name;
    TPath tarball(req.tarball());

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    tarball = clientContainer->RootPath() / tarball;

    if (tarball.GetType() != EFileType::Regular)
        return TError(EError::InvalidValue, "tarball not a file");

    if (!tarball.AccessOk(EFileAccess::Read, client->GetCred()))
        return TError(EError::Permission, "client has not read access to tarball");

    if (!layers_tmp.Exists()) {
        error = layers_tmp.Mkdir(0700);
        if (error)
            return error;
    }

    auto vholder_lock = context.Vholder->ScopedLock();
    if (layer.Exists()) {
        if (!req.merge()) {
            error = TError(EError::LayerAlreadyExists, "Layer already exists");
            goto err_tmp;
        }
        if (LayerInUse(context, layer)) {
            error = TError(EError::Busy, "layer in use");
            goto err_tmp;
        }
        error = layer.Rename(layer_tmp);
        if (error)
            goto err_tmp;
    } else {
        error = layer_tmp.Mkdir(0755);
        if (error)
            goto err_tmp;
    }
    vholder_lock.unlock();

    error = UnpackTarball(tarball, layer_tmp);
    if (error)
        goto err;

    error = SanitizeLayer(layer_tmp, req.merge());
    if (error)
        goto err;

    error = layer_tmp.Rename(layer);
    (void)layers_tmp.Rmdir();
    if (error)
        goto err;

    return TError::Success();

err:
    (void)layer_tmp.ClearDirectory();
    (void)layer_tmp.Rmdir();
err_tmp:
    (void)layers_tmp.Rmdir();
    return error;
}

noinline TError ExportLayer(TContext &context,
                            const rpc::TLayerExportRequest &req,
                            std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TContainer> clientContainer;
    error = client->GetContainer(clientContainer);
    if (error)
        return error;

    TPath tarball(req.tarball());

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    tarball = clientContainer->RootPath() / tarball;

    if (tarball.Exists())
        return TError(EError::InvalidValue, "tarball already exists");

    if (!tarball.DirName().AccessOk(EFileAccess::Write, client->GetCred()))
        return TError(EError::Permission, "client has no write access to tarball directory");

    auto vholder_lock = context.Vholder->ScopedLock();
    auto volume = context.Vholder->Find(req.volume());
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");
    error = volume->CheckPermission(client->GetCred());
    if (error)
        return error;
    vholder_lock.unlock();

    auto volume_lock = volume->ScopedLock();
    if (!volume->IsReady())
        return TError(EError::Busy, "Volume not ready");

    TPath upper;
    error = volume->GetUpperLayer(upper);
    if (error)
        return error;

    error = PackTarball(tarball, upper);
    if (error) {
        (void)tarball.Unlink();
        return error;
    }

    error = tarball.Chown(client->GetCred());
    if (error) {
        (void)tarball.Unlink();
        return error;
    }

    return TError::Success();
}

noinline TError RemoveLayer(TContext &context,
                            const rpc::TLayerRemoveRequest &req,
                            std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    TPath layers = TPath(config().volumes().layers_dir());
    TPath layer = layers / req.layer();
    if (!layer.Exists())
        return TError(EError::LayerNotFound, "Layer not found");

    TPath layers_tmp = layers / "_tmp_";
    TPath layer_tmp = layers_tmp / req.layer();
    if (!layers_tmp.Exists()) {
        error = layers_tmp.Mkdir(0700);
        if (error)
            return error;
    }

    auto vholder_lock = context.Vholder->ScopedLock();
    if (LayerInUse(context, layer)) {
        error = TError(EError::Busy, "layer in use");
        goto err;
    }
    error = layer.Rename(layer_tmp);
    if (error)
        goto err;
    vholder_lock.unlock();

    error = layer_tmp.ClearDirectory();
    if (error)
        goto err;

    error = layer_tmp.Rmdir();
err:
    (void)layers_tmp.Rmdir();
    return error;
}

noinline TError ListLayers(TContext &context,
                           rpc::TContainerResponse &rsp) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TPath layers_dir = TPath(config().volumes().layers_dir());
    std::vector<std::string> layers;

    TError error = layers_dir.ReadDirectory(layers);
    if (!error) {
        auto list = rsp.mutable_layers();
        for (auto l: layers)
            if (l != "_tmp_")
                list->add_layer(l);
    }
    return error;
}

void HandleRpcRequest(TContext &context, const rpc::TContainerRequest &req,
                      std::shared_ptr<TClient> client) {
    rpc::TContainerResponse rsp;
    string str;

    client->BeginRequest();

    bool log = config().log().verbose() || !InfoRequest(req);
    if (log) {
        std::string ns = "";
        std::shared_ptr<TContainer> clientContainer;
        TError error = client->GetContainer(clientContainer);
        if (!error)
            ns = clientContainer->GetPortoNamespace();

        L_REQ() << RequestAsString(req) << " from " << *client << " [" << ns << "]" << std::endl;
    }

    rsp.set_error(EError::Unknown);

    TError error;
    try {
        if (!ValidRequest(req)) {
            L_ERR() << "Invalid request " << req.ShortDebugString() << " from " << *client << std::endl;
            error = TError(EError::InvalidMethod, "invalid request");
        } else if (req.has_create())
            error = CreateContainer(context, req.create(), rsp, client);
        else if (req.has_destroy())
            error = DestroyContainer(context, req.destroy(), rsp, client);
        else if (req.has_list())
            error = ListContainers(context, rsp, client);
        else if (req.has_getproperty())
            error = GetContainerProperty(context, req.getproperty(), rsp, client);
        else if (req.has_setproperty())
            error = SetContainerProperty(context, req.setproperty(), rsp, client);
        else if (req.has_getdata())
            error = GetContainerData(context, req.getdata(), rsp, client);
        else if (req.has_get())
            error = GetContainerCombined(context, req.get(), rsp, client);
        else if (req.has_start())
            error = StartContainer(context, req.start(), rsp, client);
        else if (req.has_stop())
            error = StopContainer(context, req.stop(), rsp, client);
        else if (req.has_pause())
            error = PauseContainer(context, req.pause(), rsp, client);
        else if (req.has_resume())
            error = ResumeContainer(context, req.resume(), rsp, client);
        else if (req.has_propertylist())
            error = ListProperty(context, rsp);
        else if (req.has_datalist())
            error = ListData(context, rsp);
        else if (req.has_kill())
            error = Kill(context, req.kill(), rsp, client);
        else if (req.has_version())
            error = Version(context, rsp);
        else if (req.has_wait())
            error = Wait(context, req.wait(), rsp, client);
        else if (req.has_listvolumeproperties())
            error = ListVolumeProperties(context, req.listvolumeproperties(), rsp, client);
        else if (req.has_createvolume())
            error = CreateVolume(context, req.createvolume(), rsp, client);
        else if (req.has_linkvolume())
            error = LinkVolume(context, req.linkvolume(), rsp, client);
        else if (req.has_unlinkvolume())
            error = UnlinkVolume(context, req.unlinkvolume(), rsp, client);
        else if (req.has_listvolumes())
            error = ListVolumes(context, req.listvolumes(), rsp, client);
        else if (req.has_importlayer())
            error = ImportLayer(context, req.importlayer(), client);
        else if (req.has_exportlayer())
            error = ExportLayer(context, req.exportlayer(), client);
        else if (req.has_removelayer())
            error = RemoveLayer(context, req.removelayer(), client);
        else if (req.has_listlayers())
            error = ListLayers(context, rsp);
        else
            error = TError(EError::InvalidMethod, "invalid RPC method");
    } catch (std::bad_alloc exc) {
        if (config().daemon().debug())
            throw;
        rsp.Clear();
        error = TError(EError::Unknown, "memory allocation failure");
    } catch (std::string exc) {
        if (config().daemon().debug())
            throw;
        rsp.Clear();
        error = TError(EError::Unknown, exc);
    } catch (const std::exception &exc) {
        if (config().daemon().debug())
            throw;
        rsp.Clear();
        error = TError(EError::Unknown, exc.what());
    } catch (...) {
        if (config().daemon().debug())
            throw;
        rsp.Clear();
        error = TError(EError::Unknown, "unknown error");
    }

    if (error.GetError() != EError::Queued) {
        rsp.set_error(error.GetError());
        rsp.set_errormsg(error.GetMsg());
        SendReply(client, rsp, log);
    }
}
