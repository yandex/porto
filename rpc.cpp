#include "rpc.hpp"
#include "property.hpp"
#include "data.hpp"
#include "batch.hpp"
#include "container_value.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/string.hpp"
#include <algorithm>

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
        req.has_linkvolume() ||
        req.has_unlinkvolume() ||
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

    std::vector<std::string> nameVec;
    err = SplitString(name, '/', nameVec);
    if (err)
        return TError(EError::InvalidValue, "Invalid container name " + name);

    name = "";
    for (auto i = nameVec.begin(); i != nameVec.end(); i++) {
        if (!name.empty())
            name += "/";
        name += *i;

        std::shared_ptr<TContainer> container;
        err = context.Cholder->Get(name, container);
        if (err)
            return err;

        err = container->CheckPermission(client->GetCred());
        if (err)
            return err;

        if (nameVec.size() > 1)
            if (container->GetState() == EContainerState::Running ||
                container->GetState() == EContainerState::Meta)
                continue;

        std::string cmd = container->Prop->Get<std::string>(P_COMMAND);
        bool meta = i + 1 != nameVec.end() && cmd.empty();
        //bool meta = std::distance(i, nameVec.end()) == 1 && cmd.empty();
        err = container->Start(meta);
        if (err)
            return err;
    }

    return TError::Success();
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

    if (req.has_timeout()) {
        TEvent e(EEventType::WaitTimeout, nullptr);
        e.WaitTimeout.Waiter = waiter;
        context.Queue->Add(req.timeout(), e);
    }

    return TError::Queued();
}

static TError ListVolumeProperties(TContext &context,
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

static void FillVolumeDescription(rpc::TVolumeDescription *desc,
                                  std::shared_ptr<TVolume> volume) {
    desc->set_path(volume->GetPath().ToString());
    for (auto kv: volume->GetProperties()) {
        auto p = desc->add_properties();
        p->set_name(kv.first);
        p->set_value(kv.second);
    }
    for (auto name: volume->GetContainers())
        desc->add_containers(name);
}

static TError CreateVolume(TContext &context,
                           const rpc::TVolumeCreateRequest &req,
                           rpc::TContainerResponse &rsp,
                           std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
            return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::map<std::string, std::string> properties;
    for (auto p: req.properties())
        properties[p.name()] = p.value();

    std::shared_ptr<TVolume> volume;
    error = context.Vholder->Create(volume);
    if (error)
        return error;

    auto container = client->GetContainer();

    error = volume->Configure(TPath(req.has_path() ? req.path() : ""),
                              client->GetCred(), container, properties);
    if (error) {
        volume->Destroy();
        return error;
    }

    error = volume->LinkContainer(container->GetName());
    if (error) {
        volume->Destroy();
        return error;
    }
    container->LinkVolume(volume);

    error = context.Vholder->Register(volume);
    if (error) {
        volume->Destroy();
        return error;
    }

    std::weak_ptr<TClient> c = client;
    TBatchTask task(
        [volume] () {
            return volume->Build();
        },
        [volume, c] (TError error) {
            auto client = c.lock();

            if (error) {
                L_WRN() << "Can't build volume: " << error << std::endl;
                (void)volume->Destroy();
            } else if (!client) {
                L_WRN() << "Can't link volume, client is gone" << std::endl;
                (void)volume->Destroy();
            } else {
                volume->SetReady(true);
            }

            rpc::TContainerResponse response;
            response.set_error(error.GetError());
            if (!error) {
                auto desc = response.mutable_volume();
                FillVolumeDescription(desc, volume);
            }
            SendReply(client, response, true);
        });

    error = task.Run(context);
    if (error)
        return error;

    return TError::Queued();
}

static TError LinkVolume(TContext &context,
                         const rpc::TVolumeLinkRequest &req,
                         rpc::TContainerResponse &rsp,
                         std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
            return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TContainer> container;
    if (req.has_container()) {
        std::string name;
        TError err = client->GetContainer()->AbsoluteName(req.container(), name, true);
        if (err)
            return err;

        TError error = context.Cholder->Get(name, container);
        if (error)
            return error;

        error = container->CheckPermission(client->GetCred());
        if (error)
            return error;
    } else {
        container = client->GetContainer();
    }

    auto volume = context.Vholder->Find(req.path());
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");

    if (!volume->IsReady())
        return TError(EError::VolumeNotReady, "Volume not ready");

    error = volume->CheckPermission(client->GetCred());
    if (error)
        return error;

    if (!container->LinkVolume(volume))
        return TError(EError::VolumeAlreadyExists, "Already linked");

    return volume->LinkContainer(container->GetName());
}

static TError UnlinkVolume(TContext &context,
                           const rpc::TVolumeUnlinkRequest &req,
                           rpc::TContainerResponse &rsp,
                           std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
            return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::shared_ptr<TContainer> container;
    if (req.has_container()) {
        std::string name;
        TError err = client->GetContainer()->AbsoluteName(req.container(), name, true);
        if (err)
            return err;

        TError error = context.Cholder->Get(name, container);
        if (error)
            return error;

        error = container->CheckPermission(client->GetCred());
        if (error)
            return error;
    } else {
        container = client->GetContainer();
    }

    std::shared_ptr<TVolume> volume = context.Vholder->Find(req.path());
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");

    error = volume->CheckPermission(client->GetCred());
    if (error)
        return error;

    if (!container->UnlinkVolume(volume))
        return TError(EError::VolumeNotFound, "Container not linked to the volume");

    return volume->UnlinkContainer(container->GetName());
}

static TError ListVolumes(TContext &context,
                          const rpc::TVolumeListRequest &req,
                          rpc::TContainerResponse &rsp) {

    if (!config().volumes().enabled())
            return TError(EError::InvalidMethod, "volume api is disabled");

    if (req.has_path()) {
        auto volume = context.Vholder->Find(req.path());
        if (volume == nullptr)
            return TError(EError::VolumeNotFound, "volume not found");
        auto desc = rsp.mutable_volumelist()->add_volumes();
        FillVolumeDescription(desc, volume);
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

        auto desc = rsp.mutable_volumelist()->add_volumes();
        FillVolumeDescription(desc, volume);
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
        else if (req.has_listvolumeproperties())
            error = ListVolumeProperties(context, req.listvolumeproperties(), rsp, client);
        else if (req.has_createvolume())
            error = CreateVolume(context, req.createvolume(), rsp, client);
        else if (req.has_linkvolume())
            error = LinkVolume(context, req.linkvolume(), rsp, client);
        else if (req.has_unlinkvolume())
            error = UnlinkVolume(context, req.unlinkvolume(), rsp, client);
        else if (req.has_listvolumes())
            error = ListVolumes(context, req.listvolumes(), rsp);
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
