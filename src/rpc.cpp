#include <algorithm>

#include "rpc.hpp"
#include "config.hpp"
#include "version.hpp"
#include "property.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "layer.hpp"
#include "event.hpp"
#include "protobuf.hpp"
#include "helpers.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "portod.hpp"

static std::string RequestAsString(const rpc::TContainerRequest &req) {
    if (Verbose)
        return req.ShortDebugString();

    if (req.has_create())
        return std::string("create ") + req.create().name();
    else if (req.has_createweak())
        return std::string("create weak ") + req.createweak().name();
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
    else if (req.has_tunevolume()) {
        std::string ret = "volumeAPI: tune " + req.tunevolume().path();
        for (auto p: req.tunevolume().properties())
            ret += " " + p.name() + "=" + p.value();
        return ret;
    } else if (req.has_convertpath())
        return "convert " + req.convertpath().path() +
            " from " + req.convertpath().source() +
            " to " + req.convertpath().destination();
    else if (req.has_attachprocess())
        return "attach " + std::to_string(req.attachprocess().pid()) +
            " (" + req.attachprocess().comm() + ") to " +
            req.attachprocess().name();
    else
        return req.ShortDebugString();
}

static std::string ResponseAsString(const rpc::TContainerResponse &resp) {
    if (Verbose)
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
        } else if (resp.has_version()) {
            ret = resp.version().tag() + " #" + resp.version().revision();
        } else if (resp.has_wait()) {
            if (resp.wait().name().empty())
                ret = "Wait timeout";
            else
                ret = "Wait " + resp.wait().name();
        } else if (resp.has_convertpath())
            ret = resp.convertpath().path();
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
        req.has_listlayers() ||
        req.has_convertpath() ||
        req.has_getlayerprivate();
}

static bool ValidRequest(const rpc::TContainerRequest &req) {
    return
        req.has_create() +
        req.has_createweak() +
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
        req.has_tunevolume() +
        req.has_importlayer() +
        req.has_exportlayer() +
        req.has_removelayer() +
        req.has_listlayers() +
        req.has_convertpath() +
        req.has_attachprocess() +
        req.has_getlayerprivate() +
        req.has_setlayerprivate() == 1;
}

static void SendReply(TClient &client, rpc::TContainerResponse &response, bool log) {
    TError error = client.QueueResponse(response);
    if (!error) {
        if (log)
            L_RSP() << ResponseAsString(response) << " to " << client
                << " (request took " << client.GetRequestTimeMs() << "ms)"
                << std::endl;
    } else {
        L_WRN() << "Response error for " << client << " : " << error << std:: endl;
    }
}

static TError CheckPortoWriteAccess() {
    if (CL->AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "No write access at all");
    return TError::Success();
}

static noinline TError CreateContainer(std::string reqName, bool weak,
                                       rpc::TContainerResponse &rsp) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    std::string name;
    error = CL->ResolveName(reqName, name);
    if (error)
        return error;

    std::shared_ptr<TContainer> ct;
    error = TContainer::Create(name, ct);
    if (error)
        return error;

    if (!error && weak) {
        ct->IsWeak = true;
        ct->SetProp(EProperty::WEAK);

        error = ct->Save();
        if (!error)
            CL->WeakContainers.emplace_back(ct);
    }

    return error;
}

noinline TError DestroyContainer(const rpc::TContainerDestroyRequest &req,
                                 rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Destroy();
}

static noinline TError StartContainer(const rpc::TContainerStartRequest &req,
                                      rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Start();
}

noinline TError StopContainer(const rpc::TContainerStopRequest &req,
                              rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    uint64_t timeout_ms = req.has_timeout_ms() ?
        req.timeout_ms() : config().container().stop_timeout_ms();
    return ct->Stop(timeout_ms);
}

noinline TError PauseContainer(const rpc::TContainerPauseRequest &req,
                               rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    return ct->Pause();
}

noinline TError ResumeContainer(const rpc::TContainerResumeRequest &req,
                                rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Resume();
}

noinline TError ListContainers(const rpc::TContainerListRequest &req,
                               rpc::TContainerResponse &rsp) {
    std::string mask = req.has_mask() ? req.mask() : "***";
    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        std::string name;
        if (ct->IsRoot() || CL->ComposeName(ct->Name, name))
            continue;
        if (mask != "***" && !StringMatch(name, mask))
            continue;
        rsp.mutable_list()->add_name(name);
    }
    return TError::Success();
}

noinline TError GetContainerProperty(const rpc::TContainerGetPropertyRequest &req,
                                     rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (!error) {
        std::string value;
        error = ct->GetProperty(req.property(), value);
        if (!error)
            rsp.mutable_getproperty()->set_value(value);
    }
    return error;
}

noinline TError SetContainerProperty(const rpc::TContainerSetPropertyRequest &req,
                                     rpc::TContainerResponse &rsp) {
    std::string property = req.property();
    std::string value = req.value();

    /* legacy kludge */
    if (property.find('.') != std::string::npos) {
        if (property == "cpu.smart") {
            if (value == "0") {
                property = P_CPU_POLICY;
                value = "normal";
            } else {
                property = P_CPU_POLICY;
                value = "rt";
            }
        } else if (property == "memory.limit_in_bytes") {
            property = P_MEM_LIMIT;
        } else if (property == "memory.low_limit_in_bytes") {
            property = P_MEM_GUARANTEE;
        } else if (property == "memory.recharge_on_pgfault") {
            property = P_RECHARGE_ON_PGFAULT;
            value = value == "0" ? "false" : "true";
        }
    }

    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    return ct->SetProperty(property, value);
}

noinline TError GetContainerData(const rpc::TContainerGetDataRequest &req,
                                 rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (!error) {
        std::string value;
        error = ct->GetProperty(req.data(), value);
        if (!error)
            rsp.mutable_getdata()->set_value(value);
    }
    return error;
}

static void FillGetResponse(const rpc::TContainerGetRequest &req,
                            rpc::TContainerGetResponse &rsp,
                            std::string &name) {
    std::shared_ptr<TContainer> ct;

    auto lock = LockContainers();
    TError containerError = CL->ResolveContainer(name, ct);
    lock.unlock();

    auto entry = rsp.add_list();
    entry->set_name(name);
    for (int j = 0; j < req.variable_size(); j++) {
        auto var = req.variable(j);

        auto keyval = entry->add_keyval();
        std::string value;

        TError error = containerError;
        if (!error)
            error = ct->GetProperty(var, value);

        keyval->set_variable(var);
        if (error) {
            keyval->set_error(error.GetError());
            keyval->set_errormsg(error.GetMsg());
        } else {
            keyval->set_value(value);
        }
    }
}

noinline TError GetContainerCombined(const rpc::TContainerGetRequest &req,
                                     rpc::TContainerResponse &rsp) {
    bool try_lock = req.has_nonblock() && req.nonblock();
    auto get = rsp.mutable_get();
    std::list <std::string> masks, names;

    for (int i = 0; i < req.name_size(); i++) {
        auto name = req.name(i);
        if (name.find_first_of("*?") == std::string::npos)
            names.push_back(name);
        else
            masks.push_back(name);
    }

    if (!masks.empty()) {
        auto lock = LockContainers();
        for (auto &it: Containers) {
            auto &ct = it.second;
            std::string name;
            if (ct->IsRoot() || CL->ComposeName(ct->Name, name))
                continue;
            if (masks.empty())
                names.push_back(name);
            for (auto &mask: masks) {
                if (mask == "***" || StringMatch(name, mask)) {
                    names.push_back(name);
                    break;
                }
            }
        }
    }

    /* Lock all containers for read. TODO: lock only common ancestor */

    auto lock = LockContainers();
    TError error = RootContainer->LockRead(lock, try_lock);
    lock.unlock();
    if (error)
        return error;

    for (auto &name: names)
        FillGetResponse(req, *get, name);

    RootContainer->Unlock();

    return TError::Success();
}

noinline TError ListProperty(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_propertylist();
    for (auto elem : ContainerProperties) {
        if (elem.second->IsReadOnly || !elem.second->IsSupported || elem.second->IsHidden)
            continue;
        auto entry = list->add_list();
        entry->set_name(elem.first);
        entry->set_desc(elem.second->Desc.c_str());
    }
    return TError::Success();
}

noinline TError ListData(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_datalist();
    for (auto elem : ContainerProperties) {
        if (!elem.second->IsReadOnly || !elem.second->IsSupported || elem.second->IsHidden)
            continue;
        auto entry = list->add_list();
        entry->set_name(elem.first);
        entry->set_desc(elem.second->Desc.c_str());
    }
    return TError::Success();
}

noinline TError Kill(const rpc::TContainerKillRequest &req,
                     rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (error)
        return error;
    error = CL->CanControl(*ct);
    if (error)
        return error;
    return ct->Kill(req.sig());
}

noinline TError Version(rpc::TContainerResponse &rsp) {
    auto ver = rsp.mutable_version();

    ver->set_tag(PORTO_VERSION);
    ver->set_revision(PORTO_REVISION);

    return TError::Success();
}

noinline TError Wait(const rpc::TContainerWaitRequest &req,
                     rpc::TContainerResponse &rsp,
                     std::shared_ptr<TClient> &client) {
    auto lock = LockContainers();
    bool queueWait = !req.has_timeout() || req.timeout() != 0;

    if (!req.name_size())
        return TError(EError::InvalidValue, "Containers are not specified");

    auto fn = [] (std::shared_ptr<TClient> client,
                  TError error, std::string name) {
        rpc::TContainerResponse response;
        response.set_error(error.GetError());
        response.mutable_wait()->set_name(name);
        SendReply(*client, response, error || !name.empty());
    };

    auto waiter = std::make_shared<TContainerWaiter>(client, fn);

    for (int i = 0; i < req.name_size(); i++) {
        std::string name = req.name(i);
        std::string abs_name;

        if (name.find_first_of("*?") != std::string::npos) {
            waiter->Wildcards.push_back(name);
            continue;
        }

        std::shared_ptr<TContainer> ct;
        TError error = client->ResolveContainer(name, ct);
        if (error) {
            rsp.mutable_wait()->set_name(name);
            return error;
        }

        /* Explicit wait notifies non-running and hollow meta immediately */
        if (ct->State != EContainerState::Running &&
                ct->State != EContainerState::Starting &&
                (ct->State != EContainerState::Meta ||
                 !ct->RunningChildren)) {
            rsp.mutable_wait()->set_name(name);
            return TError::Success();
        }

        if (queueWait)
            ct->AddWaiter(waiter);
    }

    if (!waiter->Wildcards.empty()) {
        for (auto &it: Containers) {
            auto &ct = it.second;
            if (ct->IsRoot())
                continue;

            /* Wildcard notifies immediately only dead and hollow meta */
            if (ct->State != EContainerState::Dead &&
                    (ct->State != EContainerState::Meta ||
                     ct->RunningChildren))
                continue;

            std::string name;
            if (!client->ComposeName(ct->Name, name) &&
                    waiter->MatchWildcard(name)) {
                rsp.mutable_wait()->set_name(name);
                return TError::Success();
            }
        }

        if (queueWait)
            TContainerWaiter::AddWildcard(waiter);
    }

    if (!queueWait) {
        rsp.mutable_wait()->set_name("");
        return TError::Success();
    }

    client->Waiter = waiter;

    if (req.has_timeout()) {
        TEvent e(EEventType::WaitTimeout, nullptr);
        e.WaitTimeout.Waiter = waiter;
        EventQueue->Add(req.timeout(), e);
    }

    return TError::Queued();
}

noinline TError ConvertPath(const rpc::TConvertPathRequest &req,
                            rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> src, dst;
    TError error;

    auto lock = LockContainers();
    error = CL->ResolveContainer(
            (req.has_source() && req.source().length()) ?
            req.source() : SELF_CONTAINER, src);
    if (error)
        return error;
    error = CL->ResolveContainer(
            (req.has_destination() && req.destination().length()) ?
            req.destination() : SELF_CONTAINER, dst);
    if (error)
        return error;

    if (src == dst) {
        rsp.mutable_convertpath()->set_path(req.path());
        return TError::Success();
    }

    TPath srcRoot;
    if (src->State == EContainerState::Stopped) {
        for (auto ct = src; ct; ct = ct->Parent)
            srcRoot = ct->Root / srcRoot;
    } else
        srcRoot = src->RootPath;
    srcRoot = srcRoot.NormalPath();

    TPath dstRoot;
    if (dst->State == EContainerState::Stopped) {
        for (auto ct = dst; ct; ct = ct->Parent)
            dstRoot = ct->Root / dstRoot;
    } else
        dstRoot = dst->RootPath;
    dstRoot = dstRoot.NormalPath();

    TPath path(srcRoot / req.path());
    path = dstRoot.InnerPath(path);

    if (path.IsEmpty())
        return TError(EError::InvalidValue, "Path is unreachable");
    rsp.mutable_convertpath()->set_path(path.ToString());
    return TError::Success();
}

noinline TError ListVolumeProperties(const rpc::TVolumePropertyListRequest &req,
                                     rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_volumepropertylist();
    for (auto &prop: VolumeProperties) {
        auto p = list->add_properties();
        p->set_name(prop.Name);
        p->set_desc(prop.Desc);
    }

    return TError::Success();
}

noinline void FillVolumeDescription(rpc::TVolumeDescription *desc,
                                    TPath container_root, TPath volume_path,
                                    std::shared_ptr<TVolume> volume) {
    desc->set_path(volume_path.ToString());
    for (auto kv: volume->DumpState(container_root)) {
        auto p = desc->add_properties();
        p->set_name(kv.first);
        p->set_value(kv.second);
    }
    for (auto &name: volume->Containers) {
        std::string relative;
        if (!CL->ComposeName(name, relative))
            desc->add_containers(relative);
    }
}

noinline TError CreateVolume(const rpc::TVolumeCreateRequest &req,
                             rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(SELF_CONTAINER, ct, true);
    if (error)
        return error;

    /* FIXME unlock between create and build */
    CL->ReleaseContainer();

    TStringMap cfg;
    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    TPath volume_path;
    if (req.has_path() && !req.path().empty())
        volume_path =  ct->RootPath / req.path();

    std::shared_ptr<TVolume> volume;
    error = TVolume::Create(volume_path, cfg, *ct,
                            CL->TaskCred, volume);
    if (error)
        return error;

    volume_path = ct->RootPath.InnerPath(volume->Path, true);
    FillVolumeDescription(rsp.mutable_volume(), ct->RootPath,
                          volume_path, volume);

    return TError::Success();
}

noinline TError TuneVolume(const rpc::TVolumeTuneRequest &req,
                           rpc::TContainerResponse &rsp) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStringMap cfg;
    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    TPath volume_path = CL->ResolvePath(req.path());
    std::shared_ptr<TVolume> volume;

    error = TVolume::Find(volume_path, volume);
    if (error)
        return error;

    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return error;

    return volume->Tune(cfg);
}

noinline TError LinkVolume(const rpc::TVolumeLinkRequest &req,
                           rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.has_container() ?
                                req.container() : SELF_CONTAINER, ct, true);
    if (error)
        return error;

    TPath volume_path = CL->ResolvePath(req.path());
    std::shared_ptr<TVolume> volume;
    error = TVolume::Find(volume_path, volume);
    if (error)
        return error;
    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return error;

    return volume->LinkContainer(*ct);
}

noinline TError UnlinkVolume(const rpc::TVolumeUnlinkRequest &req,
                             rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.has_container() ?
                                req.container() : SELF_CONTAINER, ct, true);
    if (error)
        return error;

    TPath volume_path = CL->ResolvePath(req.path());
    std::shared_ptr<TVolume> volume;
    error = TVolume::Find(volume_path, volume);
    if (error)
        return error;
    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return error;

    error = volume->UnlinkContainer(*ct);

    CL->ReleaseContainer();

    if (!error && volume->IsDying)
        volume->Destroy();

    return error;
}

noinline TError ListVolumes(const rpc::TVolumeListRequest &req,
                            rpc::TContainerResponse &rsp) {
    TPath container_root = CL->ResolvePath("/");
    TError error;

    if (req.has_path() && !req.path().empty()) {
        TPath volume_path = container_root / req.path();
        std::shared_ptr<TVolume> volume;

        error = TVolume::Find(volume_path, volume);
        if (error)
            return error;

        auto desc = rsp.mutable_volumelist()->add_volumes();
        volume_path = container_root.InnerPath(volume->Path, true);
        FillVolumeDescription(desc, container_root, volume_path, volume);
        return TError::Success();
    }

    auto volumes_lock = LockVolumes();
    std::list<std::pair<TPath, std::shared_ptr<TVolume>>> list;
    for (auto &it : Volumes) {
        auto volume = it.second;

        if (req.has_container() &&
                std::find(volume->Containers.begin(), volume->Containers.end(),
                    req.container()) == volume->Containers.end())
            continue;

        TPath path = container_root.InnerPath(volume->Path, true);
        if (!path.IsEmpty())
            list.push_back(std::make_pair(path, volume));
    }
    volumes_lock.unlock();

    for (auto &it: list) {
        auto desc = rsp.mutable_volumelist()->add_volumes();
        FillVolumeDescription(desc, container_root, it.first, it.second);
    }

    return TError::Success();
}

noinline TError ImportLayer(const rpc::TLayerImportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TPath place(req.has_place() ? req.place() : PORTO_PLACE);
    error = CheckPlace(place);
    if (error)
        return error;

    TPath tarball(req.tarball());

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    tarball = CL->ResolvePath(tarball);

    if (!tarball.Exists())
        return TError(EError::InvalidValue, "tarball not found");

    if (!tarball.IsRegularFollow())
        return TError(EError::InvalidValue, "tarball not a file");

    if (!tarball.CanRead(CL->Cred))
        return TError(EError::Permission, "client has not read access to tarball");

    std::string layer_private;

    return ImportLayer(req.layer(), place, tarball, req.merge(),
                       req.has_private_value() ? req.private_value() : "",
                       CL->Cred);
}

noinline TError GetLayerPrivate(const rpc::TLayerGetPrivateRequest &req,
                                rpc::TContainerResponse &rsp) {
    TPath place(req.has_place() ? req.place() : PORTO_PLACE);
    TError error = CheckPlace(place);
    if (error)
        return error;

    std::string private_value;

    error = GetLayerPrivate(req.layer(), place, private_value);
    if (error)
        return error;

    rsp.mutable_layer_private()->set_private_value(private_value);

    return TError::Success();
}

noinline TError SetLayerPrivate(const rpc::TLayerSetPrivateRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TPath place(req.has_place() ? req.place() : PORTO_PLACE);
    error = CheckPlace(place);
    if (error)
        return error;

    return SetLayerPrivate(req.layer(), place, req.private_value());
}

noinline TError ExportLayer(const rpc::TLayerExportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TPath tarball(req.tarball());

    if (!tarball.IsAbsolute())
        return TError(EError::InvalidValue, "tarball path must be absolute");

    tarball =  CL->ResolvePath(tarball);

    if (tarball.Exists())
        return TError(EError::InvalidValue, "tarball already exists");

    if (!tarball.DirName().CanWrite(CL->Cred))
        return TError(EError::Permission, "client has no write access to tarball directory");

    auto volume = TVolume::Find(CL->ResolvePath(req.volume()));
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume not found");

    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return error;

    TPath upper;
    error = volume->GetUpperLayer(upper);
    if (error)
        return error;

    error = PackTarball(tarball, upper);
    if (error) {
        (void)tarball.Unlink();
        return error;
    }

    error = tarball.Chown(CL->Cred);
    if (error) {
        (void)tarball.Unlink();
        return error;
    }

    return TError::Success();
}

noinline TError RemoveLayer(const rpc::TLayerRemoveRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TPath place(req.has_place() ? req.place() : PORTO_PLACE);
    error = CheckPlace(place);
    if (error)
        return error;

    return RemoveLayer(req.layer(), place);
}

noinline TError ListLayers(const rpc::TLayerListRequest &req,
                           rpc::TContainerResponse &rsp) {
    TPath place(req.has_place() ? req.place() : PORTO_PLACE);
    TPath layers_dir = place / PORTO_LAYERS;
    std::vector<std::string> layers;

    TError error = layers_dir.ListSubdirs(layers);
    if (!error) {
        auto list = rsp.mutable_layers();
        for (auto &layer: layers) {
            if (LayerIsJunk(layer))
                continue;

            if (req.has_pattern() && !StringMatch(layer, req.pattern()))
                continue;

            list->add_layer(layer);

            auto desc = list->add_layers();
            desc->set_name(layer);

            std::string private_value;
            if (GetLayerPrivate(layer, place, private_value))
                private_value = "";
            desc->set_private_value(private_value);

            desc->set_last_usage(LayerLastUsage(layer, place));

            TCred owner;
            if (!LayerOwner(layer, place, owner) && owner.Uid != NoUser) {
                desc->set_owner_user(owner.User());
                desc->set_owner_group(owner.Group());
            } else {
                desc->set_owner_user("");
                desc->set_owner_group("");
            }
        }
    }
    return error;
}

noinline TError AttachProcess(const rpc::TAttachProcessRequest &req) {
    std::shared_ptr<TContainer> oldCt, newCt;
    pid_t pid = req.pid();
    TError error;

    if (pid <= 0)
        return TError(EError::InvalidValue, "invalid pid");

    error = TranslatePid(pid, CL->Pid, pid);
    if (error)
        return error;

    if (pid <= 0)
        return TError(EError::InvalidValue, "invalid pid");

    /* sanity check and protection against races */
    auto comm = StringTrim(GetTaskName(pid));
    if (StringTrim(req.comm()) != comm)
        return TError(EError::InvalidValue, "wrong task comm for pid");

    error = TContainer::FindTaskContainer(pid, oldCt);
    if (error)
        return error;

    error = CL->WriteContainer(ROOT_PORTO_NAMESPACE + oldCt->Name, oldCt);
    if (error)
        return error;

    if (pid == oldCt->Task.Pid || pid == oldCt->WaitTask.Pid ||
            pid == oldCt->SeizeTask.Pid)
        return TError(EError::Busy, "cannot move main process");

    error = CL->WriteContainer(req.name(), newCt);
    if (error)
        return error;

    if (!newCt->IsChildOf(*oldCt))
        return TError(EError::Permission, "new container must be child of current");

    if (newCt->State != EContainerState::Running &&
            newCt->State != EContainerState::Meta)
        return TError(EError::InvalidState, "new container is not running");

    for (auto ct = newCt; ct && ct != oldCt; ct = ct->Parent)
        if (ct->Isolate)
            return TError(EError::InvalidState, "new container must be not isolated from current");

    L_ACT() << "Attach process " << pid << " (" << comm << ") from "
            << oldCt->Name << " to " << newCt->Name << std::endl;

    for (auto hy: Hierarchies) {
        auto cg = newCt->GetCgroup(*hy);
        error = cg.Attach(pid);
        if (error)
            goto undo;
    }

    return TError::Success();

undo:
    for (auto hy: Hierarchies) {
        auto cg = oldCt->GetCgroup(*hy);
        (void)cg.Attach(pid);
    }
    return error;
}

void HandleRpcRequest(const rpc::TContainerRequest &req,
                      std::shared_ptr<TClient> client) {
    rpc::TContainerResponse rsp;
    std::string str;

    client->StartRequest();

    bool log = Verbose || !InfoRequest(req);
    if (log)
        L_REQ() << RequestAsString(req) << " from " << *client
                << " [" << client->ClientContainer->GetPortoNamespace() << "]" << std::endl;

    rsp.set_error(EError::Unknown);

    TError error;
    try {
        if (!ValidRequest(req)) {
            L_ERR() << "Invalid request " << req.ShortDebugString() << " from " << *client << std::endl;
            error = TError(EError::InvalidMethod, "invalid request");
        } else if (req.has_create())
            error = CreateContainer(req.create().name(), false, rsp);
        else if (req.has_createweak())
            error = CreateContainer(req.createweak().name(), true, rsp);
        else if (req.has_destroy())
            error = DestroyContainer(req.destroy(), rsp);
        else if (req.has_list())
            error = ListContainers(req.list(), rsp);
        else if (req.has_getproperty())
            error = GetContainerProperty(req.getproperty(), rsp);
        else if (req.has_setproperty())
            error = SetContainerProperty(req.setproperty(), rsp);
        else if (req.has_getdata())
            error = GetContainerData(req.getdata(), rsp);
        else if (req.has_get())
            error = GetContainerCombined(req.get(), rsp);
        else if (req.has_start())
            error = StartContainer(req.start(), rsp);
        else if (req.has_stop())
            error = StopContainer(req.stop(), rsp);
        else if (req.has_pause())
            error = PauseContainer(req.pause(), rsp);
        else if (req.has_resume())
            error = ResumeContainer(req.resume(), rsp);
        else if (req.has_propertylist())
            error = ListProperty(rsp);
        else if (req.has_datalist())
            error = ListData(rsp);
        else if (req.has_kill())
            error = Kill(req.kill(), rsp);
        else if (req.has_version())
            error = Version(rsp);
        else if (req.has_wait())
            error = Wait(req.wait(), rsp, client);
        else if (req.has_listvolumeproperties())
            error = ListVolumeProperties(req.listvolumeproperties(), rsp);
        else if (req.has_createvolume())
            error = CreateVolume(req.createvolume(), rsp);
        else if (req.has_linkvolume())
            error = LinkVolume(req.linkvolume(), rsp);
        else if (req.has_unlinkvolume())
            error = UnlinkVolume(req.unlinkvolume(), rsp);
        else if (req.has_listvolumes())
            error = ListVolumes(req.listvolumes(), rsp);
        else if (req.has_tunevolume())
            error = TuneVolume(req.tunevolume(), rsp);
        else if (req.has_importlayer())
            error = ImportLayer(req.importlayer());
        else if (req.has_exportlayer())
            error = ExportLayer(req.exportlayer());
        else if (req.has_removelayer())
            error = RemoveLayer(req.removelayer());
        else if (req.has_listlayers())
            error = ListLayers(req.listlayers(), rsp);
        else if (req.has_convertpath())
            error = ConvertPath(req.convertpath(), rsp);
        else if (req.has_attachprocess())
            error = AttachProcess(req.attachprocess());
        else if (req.has_getlayerprivate())
            error = GetLayerPrivate(req.getlayerprivate(), rsp);
        else if (req.has_setlayerprivate())
            error = SetLayerPrivate(req.setlayerprivate());
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

    client->FinishRequest();

    if (error.GetError() != EError::Queued) {
        rsp.set_error(error.GetError());
        rsp.set_errormsg(error.GetMsg());
        SendReply(*client, rsp, log);
    }
}
