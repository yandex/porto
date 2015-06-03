#include "rpc.hpp"
#include "property.hpp"
#include "data.hpp"
#include "batch.hpp"
#include "container_value.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/string.hpp"

using std::string;

static bool InfoRequest(const rpc::TContainerRequest &req) {
    if (true ||
        req.has_create() ||
        req.has_destroy() ||
        req.has_setproperty() ||
        req.has_start() ||
        req.has_stop() ||
        req.has_pause() ||
        req.has_resume() ||
        req.has_kill() ||
        req.has_createvolume() ||
        req.has_destroyvolume() ||
        req.has_wait())
        return false;

    return true;
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
        if (!WriteDelimitedTo(response, &post))
            L() << "Protobuf write error for " << client->GetFd() << std:: endl;
        else if (log)
            L_RSP() << ResponseAsString(response) << " to " << *client
                    << " (request took " << client->GetRequestTime() << "ms)" << std::endl;
        post.Flush();
    }
}

static TError CheckRequestPermissions(std::shared_ptr<TClient> client) {
    if (client->Readonly())
        return TError(EError::Permission, "Client is not a member of porto group");

    return TError::Success();
}

static TError CreateContainer(TContext &context,
                              const rpc::TContainerCreateRequest &req,
                              rpc::TContainerResponse &rsp,
                              std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;
    return context.Cholder->Create(name, client->GetCred());
}

static TError DestroyContainer(TContext &context,
                               const rpc::TContainerDestroyRequest &req,
                               rpc::TContainerResponse &rsp,
                               std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;

    { // we don't want to hold container shared_ptr because Destroy
      // might think that it has some parent that holds it
        std::shared_ptr<TContainer> container;
        TError error = context.Cholder->Get(name, container);
        if (error)
            return error;

        error = container->CheckPermission(client->GetCred());
        if (error)
            return error;
    }

    return context.Cholder->Destroy(name);
}

static TError StartContainer(TContext &context,
                             const rpc::TContainerStartRequest &req,
                             rpc::TContainerResponse &rsp,
                             std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    error = container->CheckPermission(client->GetCred());
    if (error)
        return error;

    return container->Start(false);
}

static TError StopContainer(TContext &context,
                            const rpc::TContainerStopRequest &req,
                            rpc::TContainerResponse &rsp,
                            std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    error = container->CheckPermission(client->GetCred());
    if (error)
        return error;

    return container->Stop();
}

static TError PauseContainer(TContext &context,
                             const rpc::TContainerPauseRequest &req,
                             rpc::TContainerResponse &rsp,
                             std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    error = container->CheckPermission(client->GetCred());
    if (error)
        return error;

    return container->Pause();
}

static TError ResumeContainer(TContext &context,
                              const rpc::TContainerResumeRequest &req,
                              rpc::TContainerResponse &rsp,
                              std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    error = container->CheckPermission(client->GetCred());
    if (error)
        return error;

    return container->Resume();
}

static TError ListContainers(TContext &context,
                             rpc::TContainerResponse &rsp,
                             std::shared_ptr<TClient> client) {
    for (auto c : context.Cholder->List()) {
        std::string name;
        TError err = client->GetContainer()->RelativeName(*c, name);
        if (!err)
            rsp.mutable_list()->add_name(name);
    }

    return TError::Success();
}

static TError GetContainerProperty(TContext &context,
                                   const rpc::TContainerGetPropertyRequest &req,
                                   rpc::TContainerResponse &rsp,
                                   std::shared_ptr<TClient> client) {
    std::string name;
    TError err = client->GetContainer()->AbsoluteName(req.name(), name, true);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    string value;
    error = container->GetProperty(req.property(), value);
    if (!error)
        rsp.mutable_getproperty()->set_value(value);
    return error;
}

static TError SetContainerProperty(TContext &context,
                                   const rpc::TContainerSetPropertyRequest &req,
                                   rpc::TContainerResponse &rsp,
                                   std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    error = container->CheckPermission(client->GetCred());
    if (error)
        return error;

    return container->SetProperty(req.property(), req.value(), client->GetCred().IsPrivileged());
}

static TError GetContainerData(TContext &context,
                               const rpc::TContainerGetDataRequest &req,
                               rpc::TContainerResponse &rsp,
                               std::shared_ptr<TClient> client) {
    std::string name;
    TError err = client->GetContainer()->AbsoluteName(req.name(), name, true);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    string value;
    error = container->GetData(req.data(), value);
    if (!error)
        rsp.mutable_getdata()->set_value(value);
    return error;
}

static TError GetContainerCombined(TContext &context,
                                   const rpc::TContainerGetRequest &req,
                                   rpc::TContainerResponse &rsp,
                                   std::shared_ptr<TClient> client) {
    if (!req.variable_size())
        return TError(EError::InvalidValue, "Properties/data are not specified");

    if (!req.name_size())
        return TError(EError::InvalidValue, "Containers are not specified");

    auto get = rsp.mutable_get();

    for (int i = 0; i < req.name_size(); i++) {
        auto relname = req.name(i);

        std::string name;
        std::shared_ptr<TContainer> container;
        TError containerError = client->GetContainer()->AbsoluteName(relname, name, true);
        if (!containerError)
            containerError = context.Cholder->Get(name, container);

        auto entry = get->add_list();
        entry->set_name(relname);

        for (int j = 0; j < req.variable_size(); j++) {
            auto var = req.variable(j);

            auto keyval = entry->add_keyval();
            std::string value;

            TError error = containerError;
            if (!error) {
                std::string name = var, idx;
                TContainer::ParsePropertyName(name, idx);

                if (container->Prop->IsValid(name))
                    error = container->GetProperty(var, value);
                else if (container->Data->IsValid(name))
                    error = container->GetData(var, value);
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

static TError ListProperty(TContext &context,
                           rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_propertylist();

    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(ROOT_CONTAINER, container);
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

static TError ListData(TContext &context,
                       rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_datalist();

    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(ROOT_CONTAINER, container);
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

static TError Kill(TContext &context,
                   const rpc::TContainerKillRequest &req,
                   rpc::TContainerResponse &rsp,
                   std::shared_ptr<TClient> client) {
    TError err = CheckRequestPermissions(client);
    if (err)
        return err;

    std::string name;
    err = client->GetContainer()->AbsoluteName(req.name(), name);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    error = container->CheckPermission(client->GetCred());
    if (error)
        return error;

    return container->Kill(req.sig());
}

static TError Version(TContext &context,
                      rpc::TContainerResponse &rsp) {
    auto ver = rsp.mutable_version();

    ver->set_tag(GIT_TAG);
    ver->set_revision(GIT_REVISION);

    return TError::Success();
}

static TError Wait(TContext &context,
                   const rpc::TContainerWaitRequest &req,
                   rpc::TContainerResponse &rsp,
                   std::shared_ptr<TClient> client) {
    if (!req.name_size())
        return TError(EError::InvalidValue, "Containers are not specified");

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
        TError err = client->GetContainer()->AbsoluteName(name, abs_name);
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

    return TError::Queued();
}

static TError CreateVolume(TContext &context,
                           const rpc::TVolumeCreateRequest &req,
                           rpc::TContainerResponse &rsp,
                           std::shared_ptr<TClient> client) {
    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TVolume> volume;
    volume = std::make_shared<TVolume>(context.Vholder, client->GetCred());
    error = volume->Create(context.VolumeStorage,
                           StringTrim(req.path()),
                           StringTrim(req.quota()),
                           StringTrim(req.flags()));
    if (error)
        return error;

    std::weak_ptr<TClient> c = client;
    TBatchTask task(
        [volume] () {
            return volume->Construct();
        },
        [volume, c] (TError error) {
            if (error) {
                L_WRN() << "Can't construct volume: " << error << std::endl;
                (void)volume->Destroy();
            } else {
                error = volume->SetValid(true);
                if (error) {
                    L_WRN() << "Can't mark volume valid: " << error << std::endl;
                    (void)volume->Destroy();
                }
            }

            rpc::TContainerResponse response;
            response.set_error(error.GetError());
            SendReply(c.lock(), response, true);
        });

    error = task.Run(context);
    if (error)
        return error;

    return TError::Queued();
}

static TError DestroyVolume(TContext &context,
                            const rpc::TVolumeDestroyRequest &req,
                            rpc::TContainerResponse &rsp,
                            std::shared_ptr<TClient> client) {
    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    auto volume = context.Vholder->Get(StringTrim(req.path()));
    if (volume && volume->IsValid()) {
        error = volume->CheckPermission(client->GetCred());
        if (error)
            return error;

        error = volume->SetValid(false);
        if (error) {
            L_WRN() << "Can't mark volume invalid: " << error << std::endl;
            (void)volume->Destroy();
        }

        std::weak_ptr<TClient> c = client;
        TBatchTask task(
            [volume] () {
                return volume->Deconstruct();
            },
            [volume, c] (TError error) {
                if (error)
                    L_WRN() << "Can't deconstruct volume: " << error << std::endl;

                if (!error)
                    error = volume->Destroy();

                rpc::TContainerResponse response;
                response.set_error(error.GetError());
                SendReply(c.lock(), response, true);
            });

        error = task.Run(context);
        if (error)
            return error;

        return TError::Queued();
    }

    return TError(EError::VolumeDoesNotExist, "Volume doesn't exist");
}

static TError ListVolumes(TContext &context,
                          rpc::TContainerResponse &rsp) {
    for (auto path : context.Vholder->List()) {
        auto vol = context.Vholder->Get(path);
        if (!vol->IsValid())
            continue;

        uint64_t used, avail;
        TError error = vol->GetUsage(used, avail);
        if (error)
            L_ERR() << "Can't get used for volume " << vol->GetPath() << ": " << error << std::endl;

        auto desc = rsp.mutable_volumelist()->add_list();
        desc->set_path(vol->GetPath().ToString());
        desc->set_quota(vol->GetQuota());
        desc->set_flags(vol->GetFlags());
        desc->set_used(used);
        desc->set_avail(avail);
    }

    return TError::Success();
}

void HandleRpcRequest(TContext &context, const rpc::TContainerRequest &req,
                      std::shared_ptr<TClient> client) {
    rpc::TContainerResponse rsp;
    string str;

    client->BeginRequest();

    bool log = !InfoRequest(req);
    if (log)
        L_REQ() << RequestAsString(req) << " from " << *client << std::endl;

    rsp.set_error(EError::Unknown);

    TError error;
    try {
        if (req.has_create())
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
        else if (config().volumes().enabled() && req.has_createvolume())
            error = CreateVolume(context, req.createvolume(), rsp, client);
        else if (config().volumes().enabled() && req.has_destroyvolume())
            error = DestroyVolume(context, req.destroyvolume(), rsp, client);
        else if (config().volumes().enabled() && req.has_listvolumes())
            error = ListVolumes(context, rsp);
        else
            error = TError(EError::InvalidMethod, "invalid RPC method");
    } catch (std::bad_alloc exc) {
        rsp.Clear();
        error = TError(EError::Unknown, "memory allocation failure");
    } catch (std::string exc) {
        rsp.Clear();
        error = TError(EError::Unknown, exc);
    } catch (const std::exception &exc) {
        rsp.Clear();
        error = TError(EError::Unknown, exc.what());
    } catch (...) {
        rsp.Clear();
        error = TError(EError::Unknown, "unknown error");
    }

    if (error.GetError() != EError::Queued) {
        rsp.set_error(error.GetError());
        rsp.set_errormsg(error.GetMsg());
        SendReply(client, rsp, log);
    }
}
