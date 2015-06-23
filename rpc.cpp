#include "rpc.hpp"
#include "property.hpp"
#include "data.hpp"
#include "container_value.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/string.hpp"
#include <algorithm>

using std::string;

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

static TError CreateContainer(TContext &context,
                              const rpc::TContainerCreateRequest &req,
                              rpc::TContainerResponse &rsp,
                              std::shared_ptr<TClient> client) {
    auto lock = context.Cholder->ScopedLock();

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
    auto cholder_lock = context.Cholder->ScopedLock();

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

        cholder_lock.unlock();
        auto vholder_lock = context.Vholder->ScopedLock();


        for (auto volume: container->Volumes) {
            if (!volume->UnlinkContainer(name))
                continue; /* Still linked to somebody */
            if (container->IsRoot() || container->IsPortoRoot())
                continue;
            vholder_lock.unlock();
            auto volume_lock = volume->ScopedLock();
            if (!volume->IsReady()) {
                volume_lock.unlock();
                vholder_lock.lock();
                continue;
            }
            vholder_lock.lock();
            error = volume->SetReady(false);
            vholder_lock.unlock();
            error = volume->Destroy();
            vholder_lock.lock();
            context.Vholder->Unregister(volume);
            context.Vholder->Remove(volume);
            volume_lock.unlock();
        }

        container->Volumes.clear();
        vholder_lock.unlock();

        cholder_lock.lock();
    }

    return context.Cholder->Destroy(name);
}

static TError StartContainer(TContext &context,
                             const rpc::TContainerStartRequest &req,
                             rpc::TContainerResponse &rsp,
                             std::shared_ptr<TClient> client) {
    auto lock = context.Cholder->ScopedLock();

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
        err = container->Start(client, meta);
        if (err)
            return err;
    }

    return TError::Success();
}

static TError StopContainer(TContext &context,
                            const rpc::TContainerStopRequest &req,
                            rpc::TContainerResponse &rsp,
                            std::shared_ptr<TClient> client) {
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

    std::string name;
    TError err = client->GetContainer()->AbsoluteName(req.name(), name, true);
    if (err)
        return err;
    std::shared_ptr<TContainer> container;
    TError error = context.Cholder->Get(name, container);
    if (error)
        return error;

    string value;
    error = container->GetProperty(req.property(), value, client);
    if (!error)
        rsp.mutable_getproperty()->set_value(value);
    return error;
}

static TError SetContainerProperty(TContext &context,
                                   const rpc::TContainerSetPropertyRequest &req,
                                   rpc::TContainerResponse &rsp,
                                   std::shared_ptr<TClient> client) {
    auto lock = context.Cholder->ScopedLock();

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

    return container->SetProperty(req.property(), req.value(), client);
}

static TError GetContainerData(TContext &context,
                               const rpc::TContainerGetDataRequest &req,
                               rpc::TContainerResponse &rsp,
                               std::shared_ptr<TClient> client) {
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

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
                    error = container->GetProperty(var, value, client);
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
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

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
    auto lock = context.Cholder->ScopedLock();

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

    auto vholder_lock = context.Vholder->ScopedLock();
    std::shared_ptr<TVolume> volume;
    error = context.Vholder->Create(volume);
    if (error)
        return error;

    /* cannot block: volume is not registered yet */
    auto volume_lock = volume->ScopedLock();

    auto container = client->GetContainer();
    auto container_root = container->RootPath();

    TPath volume_path("");
    if (req.has_path() && !req.path().empty())
        volume_path = container_root.AddComponent(req.path());

    error = volume->Configure(volume_path, client->GetCred(),
                              container, properties);
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

    error = volume->LinkContainer(container->GetName());
    if (error) {
        cholder_lock.unlock();

        L_WRN() << "Can't link volume" << std::endl;
        (void)volume->Destroy();
        context.Vholder->Unregister(volume);
        context.Vholder->Remove(volume);
        return error;
    }
    container->LinkVolume(volume);
    cholder_lock.unlock();

    volume->SetReady(true);
    vholder_lock.unlock();

    FillVolumeDescription(rsp.mutable_volume(), container_root, volume_path, volume);
    volume_lock.unlock();

    return TError::Success();
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

    auto cholder_lock = context.Cholder->ScopedLock();
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
    cholder_lock.unlock();

    TPath volume_path = client->GetContainer()->RootPath().AddComponent(req.path());

    auto vholder_lock = context.Vholder->ScopedLock();
    auto volume = context.Vholder->Find(volume_path);

    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");
    vholder_lock.unlock();

    auto volume_lock = volume->ScopedLock();
    if (!volume->IsReady())
        return TError(EError::VolumeNotReady, "Volume not ready");

    error = volume->CheckPermission(client->GetCred());
    if (error)
        return error;

    vholder_lock.lock();
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

    auto vholder_lock = context.Vholder->ScopedLock();
    auto cholder_lock = context.Cholder->ScopedLock();

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

    TPath volume_path = client->GetContainer()->RootPath().AddComponent(req.path());

    std::shared_ptr<TVolume> volume = context.Vholder->Find(volume_path);
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");

    error = volume->CheckPermission(client->GetCred());
    if (error)
        return error;

    if (!container->UnlinkVolume(volume))
        return TError(EError::VolumeNotFound, "Container not linked to the volume");

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
    if (!error) {
        vholder_lock.lock();
        context.Vholder->Unregister(volume);
        context.Vholder->Remove(volume);
        vholder_lock.unlock();
    }
    volume_lock.unlock();

    return error;
}

static TError ListVolumes(TContext &context,
                          const rpc::TVolumeListRequest &req,
                          rpc::TContainerResponse &rsp,
                          std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
            return TError(EError::InvalidMethod, "volume api is disabled");

    auto container = client->GetContainer();
    TPath container_root = container->RootPath();

    auto vholder_lock = context.Vholder->ScopedLock();

    if (req.has_path() && !req.path().empty()) {
        TPath volume_path = container_root.AddComponent(req.path());
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

static TError ImportLayer(TContext &context,
                          const rpc::TLayerImportRequest &req,
                          std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    std::string layer_name = req.layer();
    if (layer_name.find_first_of("/\\\n\r\t ") != string::npos ||
            layer_name == "_tmp_")
        return TError(EError::InvalidValue, "invalid layer name");

    TPath layers = TPath(config().volumes().layers_dir());
    TPath layers_tmp = layers.AddComponent("_tmp_");
    TPath layer = layers.AddComponent(layer_name);
    TPath layer_tmp = layers_tmp.AddComponent(layer_name);
    TPath tarball(req.tarball());

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    tarball = client->GetContainer()->RootPath().AddComponent(tarball);

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
            error = TError(EError::InvalidValue, "layer already exists");
            goto err_tmp;
        }
        if (LayerInUse(context, layer)) {
            error = TError(EError::VolumeIsBusy, "layer in use");
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

static TError ExportLayer(TContext &context,
                          const rpc::TLayerExportRequest &req,
                          std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
        return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    TPath tarball(req.tarball());

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    tarball = client->GetContainer()->RootPath().AddComponent(tarball);

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
        return TError(EError::VolumeNotReady, "Volume not ready");

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

static TError RemoveLayer(TContext &context,
                          const rpc::TLayerRemoveRequest &req,
                          std::shared_ptr<TClient> client) {

    if (!config().volumes().enabled())
            return TError(EError::InvalidMethod, "volume api is disabled");

    TError error = CheckRequestPermissions(client);
    if (error)
        return error;

    TPath layers = TPath(config().volumes().layers_dir());
    TPath layer = layers.AddComponent(req.layer());
    if (!layer.Exists())
        return TError(EError::InvalidValue, "layer does not exist");

    TPath layers_tmp = layers.AddComponent("_tmp_");
    TPath layer_tmp = layers_tmp.AddComponent(req.layer());
    if (!layers_tmp.Exists()) {
        error = layers_tmp.Mkdir(0700);
        if (error)
            return error;
    }

    auto vholder_lock = context.Vholder->ScopedLock();
    if (LayerInUse(context, layer)) {
        error = TError(EError::VolumeIsBusy, "layer in use");
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

static TError ListLayers(TContext &context,
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
