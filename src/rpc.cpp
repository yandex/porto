#include <algorithm>

#include "rpc.hpp"
#include "config.hpp"
#include "version.hpp"
#include "property.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "event.hpp"
#include "protobuf.hpp"
#include "helpers.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "portod.hpp"
#include "storage.hpp"

extern "C" {
#include <sys/stat.h>
}

static void RequestToString(const rpc::TContainerRequest &req,
                            std::string &cmd, std::string &arg,
                            std::vector<std::string> &opts) {
    if (req.has_create()) {
        cmd = "Create";
        arg = req.create().name();
    } else if (req.has_createweak()) {
        cmd = "Create";
        arg = req.createweak().name();
        opts = { "weak=true" };
    } else if (req.has_destroy()) {
        cmd = "Destroy";
        arg = req.destroy().name();
    } else if (req.has_list()) {
        cmd = "List";
        if (req.list().has_mask())
            opts = { "mas=" + req.list().mask() };
    } else if (req.has_getproperty()) {
        cmd = "Get";
        arg = req.getproperty().name();
        opts = { "property=" + req.getproperty().property() };
        if (req.getproperty().has_sync() && req.getproperty().sync())
            opts.push_back("sync=true");
        if (req.getproperty().has_real() && req.getproperty().real())
            opts.push_back("real=true");
    } else if (req.has_getdata()) {
        cmd = "Get";
        arg = req.getdata().name();
        opts = { "property=" + req.getdata().data() };
        if (req.getdata().has_sync() && req.getdata().sync())
            opts.push_back("sync=true");
        if (req.getdata().has_real() && req.getdata().real())
            opts.push_back("real=true");
    } else if (req.has_get()) {
        cmd = "Get";
        for (int i = 0; i < req.get().name_size(); i++)
            opts.push_back(req.get().name(i));
        opts.push_back("--");
        for (int i = 0; i < req.get().variable_size(); i++)
            opts.push_back(req.get().variable(i));
        if (req.get().has_nonblock() && req.get().nonblock())
            opts.push_back("nonblock=true");
        if (req.get().has_sync() && req.get().sync())
            opts.push_back("sync=true");
        if (req.get().has_real() && req.get().real())
            opts.push_back("real=true");
    } else if (req.has_setproperty()) {
        cmd = "Set";
        arg = req.setproperty().name();
        opts = { req.setproperty().property() + "=" + req.setproperty().value() };
    } else if (req.has_start()) {
        cmd = "Start";
        arg = req.start().name();
    } else if (req.has_stop()) {
        cmd = "Stop";
        arg = req.stop().name();
        if (req.stop().has_timeout_ms())
            opts.push_back(fmt::format("timeout={} ms", req.stop().timeout_ms()));
    } else if (req.has_pause()) {
        cmd = "Pause";
        arg = req.pause().name();
    } else if (req.has_resume()) {
        cmd = "Resume";
        arg = req.resume().name();
    } else if (req.has_respawn()) {
        cmd = "Respawn";
        arg = req.respawn().name();
    } else if (req.has_wait()) {
        cmd = "Wait";
        for (int i = 0; i < req.wait().name_size(); i++)
            opts.push_back(req.wait().name(i));
        if (req.wait().has_timeout_ms())
            opts.push_back(fmt::format("timeout={} ms", req.wait().timeout_ms()));
    } else if (req.has_propertylist() || req.has_datalist()) {
        cmd = "ListProperties";
    } else if (req.has_kill()) {
        cmd = "Kill";
        arg = req.kill().name();
        opts = { fmt::format("signal={}", req.kill().sig()) };
    } else if (req.has_version()) {
        cmd = "Version";
    } else if (req.has_createvolume()) {
        cmd = "CreateVolume";
        arg = req.createvolume().path();
        for (auto p: req.createvolume().properties())
            opts.push_back(p.name() + "=" + p.value());
    } else if (req.has_linkvolume()) {
        cmd = "LinkVolume";
        arg = req.linkvolume().path();
        opts = { "container=" + req.linkvolume().container() };
    } else if (req.has_linkvolumetarget()) {
        cmd = "LinkVolume";
        arg = req.linkvolumetarget().path();
        opts = { "container=" + req.linkvolumetarget().container() };
        if (req.linkvolumetarget().target() != "")
            opts.push_back("target=" + req.linkvolumetarget().target());
        if (req.linkvolumetarget().required())
            opts.push_back("required=true");
    } else if (req.has_unlinkvolume()) {
        cmd = "UnlinkVolume";
        arg = req.unlinkvolume().path();
        opts = { "container=" + req.unlinkvolume().container() };
        if (req.unlinkvolume().has_strict() && req.unlinkvolume().strict())
            opts.push_back("strict=true");
    } else if (req.has_listvolumes()) {
        cmd = "ListVolumes";
        if (req.listvolumes().has_path())
            arg = req.listvolumes().path();
        if (req.listvolumes().has_container())
            opts = { "container=" + req.listvolumes().container() };
    } else if (req.has_tunevolume()) {
        cmd = "TuneVolume";
        arg = req.tunevolume().path();
        for (auto p: req.tunevolume().properties())
            opts.push_back(p.name() + "=" + p.value());
    } else if (req.has_listvolumeproperties()) {
        cmd = "ListVolumeProperities";
    } else if (req.has_importlayer()) {
        cmd = "ImportLayer";
        arg = req.importlayer().layer();
        opts = { "tarball=" + req.importlayer().tarball() };
        if (req.importlayer().has_compress())
            opts.push_back("compress=" + req.importlayer().compress());
        if (req.importlayer().has_place())
            opts.push_back("place=" + req.importlayer().place());
        if (req.importlayer().has_merge())
            opts.push_back("merge=true");
        if (req.importlayer().has_private_value())
            opts.push_back("private=" + req.importlayer().private_value());
    } else if (req.has_exportlayer()) {
        if (req.exportlayer().has_layer()) {
            cmd = "ReexportLayer";
            arg = req.exportlayer().layer();
        } else {
            cmd = "ExportLayer";
            arg = req.exportlayer().volume();
        }
        opts = { "tarball+" + req.exportlayer().tarball() };
        if (req.exportlayer().has_compress())
            opts.push_back("compress=" + req.exportlayer().compress());
        if (req.exportlayer().has_place())
            opts.push_back("place=" + req.exportlayer().place());
    } else if (req.has_removelayer()) {
        cmd = "RemoveLayer";
        arg = req.removelayer().layer();
        if (req.removelayer().has_place())
            opts.push_back("place=" + req.removelayer().place());
    } else if (req.has_listlayers()) {
        cmd = "ListLayers";
        if (req.listlayers().has_mask())
            opts.push_back("mask=" + req.listlayers().mask());
        if (req.listlayers().has_place())
            opts.push_back("place=" + req.listlayers().place());
    } else if (req.has_getlayerprivate()) {
        cmd = "GetLayerPrivate";
    } else if (req.has_setlayerprivate()) {
        cmd = "SetLayerPrivate";
    } else if (req.has_convertpath()) {
        cmd = "ConvertPath";
        arg = req.convertpath().path();
        opts = { "source=" + req.convertpath().source(), "destination=" + req.convertpath().destination() };
    } else if (req.has_attachprocess()) {
        cmd = "AttachProcess";
        arg = req.attachprocess().name();
        opts = { "pid=" + std::to_string(req.attachprocess().pid()), "comm=" + req.attachprocess().comm() };
    } else if (req.has_locateprocess()) {
        cmd = "LocateProcess";
        opts = { "pid=" + std::to_string(req.locateprocess().pid()), "comm=" + req.locateprocess().comm() };
    } else if (req.has_getsystem()) {
        cmd = "GetSystem";
    } else if (req.has_setsystem()) {
        cmd = "SetSystem";
        arg = req.ShortDebugString();
    } else {
        cmd = "UnknownCommand";
        arg = req.ShortDebugString();
    }
}

static std::string ResponseAsString(const rpc::TContainerResponse &resp) {
    std::string ret;

    if (resp.error()) {
        ret = fmt::format("Error {}:{}({})", resp.error(),
                          rpc::EError_Name(resp.error()), resp.errormsg());
    } else if (resp.has_list()) {
        for (int i = 0; i < resp.list().name_size(); i++)
            ret += resp.list().name(i) + " ";
    } else if (resp.has_propertylist()) {
        for (int i = 0; i < resp.propertylist().list_size(); i++)
            ret += resp.propertylist().list(i).name()
                + " (" + resp.propertylist().list(i).desc() + ")";
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

            for (int j = 0; j < entry.keyval_size(); j++) {
                auto &val = entry.keyval(j);
                ret += " " + val.variable() + "=";
                if (val.has_error())
                    ret += fmt::format("{}:{}({})", val.error(),
                                       rpc::EError_Name(val.error()),
                                       val.errormsg());
                else if (val.has_value())
                    ret += val.value();
            }
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
}

/* not logged in normal mode */
static bool SilentRequest(const rpc::TContainerRequest &req) {
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
        req.has_getlayerprivate() ||
        req.has_liststorage() ||
        req.has_locateprocess() ||
        req.has_getsystem();
}

static TError CheckPortoWriteAccess() {
    if (CL->AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "Write access denied");
    return OK;
}

static noinline TError CreateContainer(std::string reqName, bool weak) {
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

noinline TError DestroyContainer(const rpc::TContainerDestroyRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Destroy();
}

static noinline TError StartContainer(const rpc::TContainerStartRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Start();
}

noinline TError StopContainer(const rpc::TContainerStopRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    uint64_t timeout_ms = req.has_timeout_ms() ?
        req.timeout_ms() : config().container().stop_timeout_ms();
    return ct->Stop(timeout_ms);
}

noinline TError PauseContainer(const rpc::TContainerPauseRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    return ct->Pause();
}

noinline TError ResumeContainer(const rpc::TContainerResumeRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Resume();
}

noinline TError RespawnContainer(const rpc::TContainerRespawnRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;
    return ct->Respawn();
}

noinline TError ListContainers(const rpc::TContainerListRequest &req,
                               rpc::TContainerResponse &rsp) {
    std::string mask = req.has_mask() ? req.mask() : "***";
    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        std::string name;
        if (ct->IsRoot() || CL->ComposeName(ct->Name, name) ||
                !StringMatch(name, mask))
            continue;
        rsp.mutable_list()->add_name(name);
    }
    return OK;
}

noinline TError GetContainerProperty(const rpc::TContainerGetPropertyRequest &req,
                                     rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (!error) {
        std::string value;

        if (req.has_real() && req.real()) {
            error = ct->HasProperty(req.property());
            if (error)
                return error;
        }

        if (req.has_sync() && req.sync())
            ct->SyncProperty(req.property());

        error = ct->GetProperty(req.property(), value);
        if (!error)
            rsp.mutable_getproperty()->set_value(value);
    }
    return error;
}

noinline TError SetContainerProperty(const rpc::TContainerSetPropertyRequest &req) {
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

        if (req.has_real() && req.real()) {
            error = ct->HasProperty(req.data());
            if (error)
                return error;
        }

        if (req.has_sync() && req.sync())
            ct->SyncProperty(req.data());

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
        if (!error && req.has_real() && req.real())
            error = ct->HasProperty(var);
        if (!error)
            error = ct->GetProperty(var, value);

        keyval->set_variable(var);
        if (error) {
            keyval->set_error(error.Error);
            keyval->set_errormsg(error.Message());
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
                if (StringMatch(name, mask)) {
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

    if (req.has_sync() && req.sync())
        TContainer::SyncPropertiesAll();

    for (auto &name: names)
        FillGetResponse(req, *get, name);

    RootContainer->Unlock();

    return OK;
}

noinline TError ListProperty(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_propertylist();
    for (auto &elem : ContainerProperties) {
        auto &prop = elem.second;
        if (!prop->IsSupported || prop->IsHidden)
            continue;
        auto entry = list->add_list();
        entry->set_name(prop->Name);
        entry->set_desc(prop->GetDesc());
        entry->set_read_only(prop->IsReadOnly);
        entry->set_dynamic(prop->IsDynamic);
    }
    return OK;
}

noinline TError Kill(const rpc::TContainerKillRequest &req) {
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

    return OK;
}

noinline TError Wait(const rpc::TContainerWaitRequest &req,
                     rpc::TContainerResponse &rsp,
                     std::shared_ptr<TClient> &client) {
    auto lock = LockContainers();
    bool queueWait = !req.has_timeout_ms() || req.timeout_ms() != 0;

    if (!req.name_size())
        return TError(EError::InvalidValue, "Containers are not specified");

    auto waiter = std::make_shared<TContainerWaiter>(client);

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
            return OK;
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
                return OK;
            }
        }

        if (queueWait)
            TContainerWaiter::AddWildcard(waiter);
    }

    if (!queueWait) {
        rsp.mutable_wait()->set_name("");
        return OK;
    }

    client->Waiter = waiter;

    if (req.has_timeout_ms()) {
        TEvent e(EEventType::WaitTimeout, nullptr);
        e.WaitTimeout.Waiter = waiter;
        EventQueue->Add(req.timeout_ms(), e);
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
        return OK;
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
    return OK;
}

noinline TError ListVolumeProperties(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_volumepropertylist();
    for (auto &prop: VolumeProperties) {
        auto p = list->add_properties();
        p->set_name(prop.Name);
        p->set_desc(prop.Desc);
    }

    return OK;
}

noinline static void
FillVolumeDescription(TVolume &volume, const TPath &path, rpc::TVolumeDescription *desc) {

    if (path)
        desc->set_path(path.ToString());
    else
        desc->set_path(CL->ComposePath(volume.Path).ToString());

    TStringMap props;
    TStringMap links;
    volume.Dump(props, links);

    for (auto &kv: props) {
        auto p = desc->add_properties();
        p->set_name(kv.first);
        p->set_value(kv.second);
    }

    for (auto &it: links) {
        desc->add_containers(it.first);
        auto link = desc->add_links();
        link->set_container(it.first);
        link->set_target(it.second);
    }
}

noinline TError CreateVolume(const rpc::TVolumeCreateRequest &req,
                             rpc::TContainerResponse &rsp) {
    TStringMap cfg;

    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    if (req.has_path() && !req.path().empty())
        cfg[V_PATH] = req.path();

    if (!cfg.count(V_PLACE) && CL->DefaultPlace() != PORTO_PLACE)
        cfg[V_PLACE] = CL->DefaultPlace().ToString();

    std::shared_ptr<TVolume> volume;
    Statistics->VolumesCreated++;
    TError error = TVolume::Create(cfg, volume);
    if (error) {
        Statistics->VolumesFailed++;
        return error;
    }

    FillVolumeDescription(*volume, "", rsp.mutable_volume());
    return OK;
}

noinline TError TuneVolume(const rpc::TVolumeTuneRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStringMap cfg;
    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    std::shared_ptr<TVolume> volume;
    error = TVolume::Resolve(*CL->ClientContainer, req.path(), volume);
    if (error)
        return error;

    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return TError(error, "Cannot tune volume {}", volume->Path);

    return volume->Tune(cfg);
}

noinline TError LinkVolume(const rpc::TVolumeLinkRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.container() != "" ? req.container() : SELF_CONTAINER, ct, true);
    if (error)
        return error;

    std::shared_ptr<TVolume> volume;
    error = TVolume::Resolve(*CL->ClientContainer, req.path(), volume);
    if (error)
        return error;
    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return TError(error, "Cannot link volume {}", volume->Path);

    error = volume->LinkContainer(*ct, req.target());
    if (error)
        return error;

    if (req.required()) {
        auto lock = LockVolumes();
        auto &paths = ct->RequiredVolumes;
        if (std::find(paths.begin(), paths.end(), volume->Path) == paths.end()) {
            paths.push_back(volume->Path.ToString());
            volume->HasDependentContainer = true;
            lock.unlock();
            ct->SetProp(EProperty::REQUIRED_VOLUMES);
            ct->Save();
        }
    }

    return OK;
}

noinline TError UnlinkVolume(const rpc::TVolumeUnlinkRequest &req) {
    bool strict = req.has_strict() && req.strict();
    std::shared_ptr<TContainer> ct;
    TError error;

    if (!req.has_container() || req.container() != "***") {
        error = CL->WriteContainer(req.has_container() ? req.container() :
                                    SELF_CONTAINER, ct, true);
        if (error)
            return error;
    }

    std::shared_ptr<TVolume> volume;
    error = TVolume::Resolve(*CL->ClientContainer, req.path(), volume);
    if (error)
        return error;
    error = CL->CanControl(volume->VolumeOwner);
    if (error)
        return TError(error, "Cannot unlink volume {}", volume->Path);

    if (ct) {
        error = volume->UnlinkContainer(*ct, strict);
        CL->ReleaseContainer();
    } else {
        error = volume->Destroy(strict);
    }

    return error;
}

noinline TError ListVolumes(const rpc::TVolumeListRequest &req,
                            rpc::TContainerResponse &rsp) {
    TError error;

    if (req.has_path() && !req.path().empty()) {
        std::shared_ptr<TVolume> volume;

        error = TVolume::Resolve(*CL->ClientContainer, req.path(), volume);
        if (error)
            return error;

        auto desc = rsp.mutable_volumelist()->add_volumes();
        FillVolumeDescription(*volume, req.path(), desc);
        return OK;
    }

    auto volumes_lock = LockVolumes();
    std::list<std::pair<std::shared_ptr<TVolume>, TPath>> list;
    for (auto &it : Volumes) {
        auto volume = it.second;

        if (req.has_container() && !volume->Links.count(req.container()))
            continue;

        auto path = volume->Compose(*CL->ClientContainer);
        if (path)
            list.emplace_back(volume, path);
    }
    volumes_lock.unlock();

    for (auto &pair: list)
        FillVolumeDescription(*pair.first, pair.second,
                              rsp.mutable_volumelist()->add_volumes());

    return OK;
}

noinline TError ImportLayer(const rpc::TLayerImportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());

    if (req.has_private_value())
        layer.Private = req.private_value();

    layer.Owner = CL->Cred;

    return layer.ImportArchive(CL->ResolvePath(req.tarball()),
                               req.has_compress() ? req.compress() : "",
                               req.merge());
}

noinline TError GetLayerPrivate(const rpc::TLayerGetPrivateRequest &req,
                                rpc::TContainerResponse &rsp) {
    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());
    TError error = CL->CanControlPlace(layer.Place);
    if (error)
        return error;
    error = layer.Load();
    if (!error)
        rsp.mutable_layer_private()->set_private_value(layer.Private);
    return error;
}

noinline TError SetLayerPrivate(const rpc::TLayerSetPrivateRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());
    error = CL->CanControlPlace(layer.Place);
    if (error)
        return error;
    error = layer.Load();
    if (error)
        return error;
    error = CL->CanControl(layer.Owner);
    if (error)
        return TError(error, "Cannot set layer private {}", layer.Name);
    return layer.SetPrivate(req.private_value());
}

noinline TError ExportLayer(const rpc::TLayerExportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    if (req.has_layer()) {
        TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                       PORTO_LAYERS, req.layer());

        error = CL->CanControlPlace(layer.Place);
        if (error)
            return error;

        error = layer.Load();
        if (error)
            return error;

        return layer.ExportArchive(CL->ResolvePath(req.tarball()),
                                   req.has_compress() ? req.compress() : "");
    }

    std::shared_ptr<TVolume> volume;
    error = TVolume::Resolve(*CL->ClientContainer, req.volume(), volume);
    if (error)
        return error;

    TStorage layer(volume->Place, PORTO_VOLUMES, volume->Id);
    layer.Owner = volume->VolumeOwner;
    error = volume->GetUpperLayer(layer.Path);
    if (error)
        return error;

    return layer.ExportArchive(CL->ResolvePath(req.tarball()),
                               req.has_compress() ? req.compress() : "");
}

noinline TError RemoveLayer(const rpc::TLayerRemoveRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage layer(req.has_place() ? req.place() : CL->DefaultPlace(),
                   PORTO_LAYERS, req.layer());
    return layer.Remove();
}

noinline TError ListLayers(const rpc::TLayerListRequest &req,
                           rpc::TContainerResponse &rsp) {
    TPath place = req.has_place() ? req.place() : CL->DefaultPlace();
    TError error = CL->CanControlPlace(place);
    if (error)
        return error;

    std::list<TStorage> layers;
    error = TStorage::List(place, PORTO_LAYERS, layers);
    if (error)
        return error;

    auto list = rsp.mutable_layers();
    for (auto &layer: layers) {
        if (req.has_mask() && !StringMatch(layer.Name, req.mask()))
            continue;
        list->add_layer(layer.Name);
        (void)layer.Load();
        auto desc = list->add_layers();
        desc->set_name(layer.Name);
        desc->set_owner_user(layer.Owner.User());
        desc->set_owner_group(layer.Owner.Group());
        desc->set_private_value(layer.Private);
        desc->set_last_usage(layer.LastUsage());
    }

    return error;
}

noinline TError AttachProcess(const rpc::TAttachProcessRequest &req) {
    std::shared_ptr<TContainer> oldCt, newCt;
    pid_t pid = req.pid();
    std::string comm;
    TError error;

    if (pid <= 0)
        return TError(EError::InvalidValue, "invalid pid");

    error = TranslatePid(pid, CL->Pid, pid);
    if (error)
        return error;

    if (pid <= 0)
        return TError(EError::InvalidValue, "invalid pid");

    comm = GetTaskName(pid);

    /* sanity check and protection against races */
    if (req.comm().size() && req.comm() != comm)
        return TError(EError::InvalidValue, "wrong task comm for pid");

    error = TContainer::FindTaskContainer(pid, oldCt);
    if (error)
        return error;

    if (oldCt == CL->ClientContainer)
        error = CL->WriteContainer(SELF_CONTAINER, oldCt, true);
    else
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

    L_ACT("Attach process {} ({}) from {} to {}", pid, comm,
          oldCt->Name, newCt->Name);

    for (auto hy: Hierarchies) {
        auto cg = newCt->GetCgroup(*hy);
        error = cg.Attach(pid);
        if (error)
            goto undo;
    }

    return OK;

undo:
    for (auto hy: Hierarchies) {
        auto cg = oldCt->GetCgroup(*hy);
        (void)cg.Attach(pid);
    }
    return error;
}

noinline TError LocateProcess(const rpc::TLocateProcessRequest &req,
                              rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    pid_t pid = req.pid();
    std::string name;

    if (pid <= 0 || TranslatePid(pid, CL->Pid, pid))
        return TError(EError::InvalidValue, "wrong pid");

    if (req.comm().size() && req.comm() != GetTaskName(pid))
        return TError(EError::InvalidValue, "wrong comm");

    if (TContainer::FindTaskContainer(pid, ct))
        return TError(EError::InvalidValue, "task not found");

    if (CL->ComposeName(ct->Name, name)) {
        if (CL->ClientContainer == ct)
            name = SELF_CONTAINER;
        else
            return TError(EError::Permission, "container is unreachable");
    }

    rsp.mutable_locateprocess()->set_name(name);

    return OK;
}

noinline TError ListStorage(const rpc::TStorageListRequest &req,
                            rpc::TContainerResponse &rsp) {
    TPath place = req.has_place() ? req.place() : CL->DefaultPlace();
    TError error = CL->CanControlPlace(place);
    if (error)
        return error;

    std::list<TStorage> storages;
    error = TStorage::List(place, PORTO_STORAGE, storages);
    if (error)
        return error;

    auto list = rsp.mutable_storagelist();
    for (auto &storage: storages) {
        if (req.has_mask() && !StringMatch(storage.Name, req.mask()))
            continue;
        if (storage.Load())
            continue;
        auto desc = list->add_storages();
        desc->set_name(storage.Name);
        desc->set_owner_user(storage.Owner.User());
        desc->set_owner_group(storage.Owner.Group());
        desc->set_private_value(storage.Private);
        desc->set_last_usage(storage.LastUsage());
    }

    return error;
}

noinline TError RemoveStorage(const rpc::TStorageRemoveRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage storage(req.has_place() ? req.place() : CL->DefaultPlace(),
                     PORTO_STORAGE, req.name());
    return storage.Remove();
}

noinline TError ImportStorage(const rpc::TStorageImportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage storage(req.has_place() ? req.place() : CL->DefaultPlace(),
                     PORTO_STORAGE, req.name());
    storage.Owner = CL->Cred;
    if (req.has_private_value())
        storage.Private = req.private_value();

    return storage.ImportArchive(CL->ResolvePath(req.tarball()),
                                 req.has_compress() ? req.compress() : "");
}

noinline TError ExportStorage(const rpc::TStorageExportRequest &req) {
    TError error = CheckPortoWriteAccess();
    if (error)
        return error;

    TStorage storage(req.has_place() ? req.place() : CL->DefaultPlace(),
                     PORTO_STORAGE, req.name());

    error = CL->CanControlPlace(storage.Place);
    if (error)
        return error;

    error = storage.Load();
    if (error)
        return error;

    return storage.ExportArchive(CL->ResolvePath(req.tarball()),
                                 req.has_compress() ? req.compress() : "");
}

noinline static TError GetSystemProperties(const rpc::TGetSystemRequest *req, rpc::TGetSystemResponse *rsp) {
    rsp->set_porto_version(PORTO_VERSION);
    rsp->set_porto_revision(PORTO_REVISION);
    rsp->set_kernel_version(config().linux_version());
    rsp->set_errors(Statistics->Errors);
    rsp->set_warnings(Statistics->Warns);
    rsp->set_restarts(Statistics->Spawned - 1);
    rsp->set_porto_uptime((GetCurrentTimeMs() - Statistics->PortoStarted) / 1000);
    rsp->set_master_uptime((GetCurrentTimeMs() - Statistics->MasterStarted) / 1000);

    rsp->set_verbose(Verbose);
    rsp->set_debug(Debug);
    rsp->set_log_lines(Statistics->LogLines);
    rsp->set_log_bytes(Statistics->LogBytes);

    rsp->set_container_count(Statistics->ContainersCount - NR_SERVICE_CONTAINERS);
    rsp->set_container_limit(config().container().max_total());
    rsp->set_container_running(RootContainer->RunningChildren);
    rsp->set_container_created(Statistics->ContainersCreated);
    rsp->set_container_started(Statistics->ContainersStarted);
    rsp->set_container_start_failed(Statistics->ContainersFailedStart);
    rsp->set_container_oom(Statistics->ContainersOOM);
    rsp->set_container_buried(Statistics->RemoveDead);
    rsp->set_container_lost(Statistics->RestoreFailed);

    rsp->set_stream_rotate_bytes(Statistics->LogRotateBytes);
    rsp->set_stream_rotate_errors(Statistics->LogRotateErrors);

    rsp->set_volume_count(Statistics->VolumesCount);
    rsp->set_volume_limit(config().volumes().max_total());
    rsp->set_volume_created(Statistics->VolumesCreated);
    rsp->set_volume_failed(Statistics->VolumesFailed);

    rsp->set_client_count(Statistics->ClientsCount);
    rsp->set_client_max(config().daemon().max_clients());
    rsp->set_client_connected(Statistics->ClientsConnected);

    rsp->set_request_queued(Statistics->RequestsQueued);
    rsp->set_request_completed(Statistics->RequestsCompleted);
    rsp->set_request_failed(Statistics->RequestsFailed);
    rsp->set_request_threads(config().daemon().workers());
    rsp->set_request_longer_1s(Statistics->RequestsLonger1s);
    rsp->set_request_longer_3s(Statistics->RequestsLonger3s);
    rsp->set_request_longer_30s(Statistics->RequestsLonger30s);
    rsp->set_request_longer_5m(Statistics->RequestsLonger5m);

    rsp->set_fail_system(Statistics->FailSystem);
    rsp->set_fail_invalid_value(Statistics->FailInvalidValue);
    rsp->set_fail_invalid_command(Statistics->FailInvalidCommand);

    rsp->set_network_count(Statistics->NetworksCount);

    return OK;
}

noinline static TError SetSystemProperties(const rpc::TSetSystemRequest *req, rpc::TSetSystemResponse *rsp) {
    if (!CL->IsSuperUser())
        return TError(EError::Permission, "Only for super-user");

    if (req->has_verbose()) {
        Verbose = req->verbose();
        Debug &= Verbose;
    }

    if (req->has_debug()) {
        Debug = req->debug();
        Verbose |= Debug;
    }

    return OK;
}

static TError CheckRpcRequest(const rpc::TContainerRequest &req) {
    auto req_ref = req.GetReflection();

    std::vector<const google::protobuf::FieldDescriptor *> req_fields;
    req_ref->ListFields(req, &req_fields);

    if (req_fields.size() != 1)
        return TError(EError::InvalidMethod, "Request has {} known methods", req_fields.size());

    auto msg = &req_ref->GetMessage(req, req_fields[0]);
    auto msg_ref = msg->GetReflection();
    auto msg_unknown = &msg_ref->GetUnknownFields(*msg);

    if (msg_unknown->field_count() != 0)
        return TError(EError::InvalidMethod, "Request has {} unknown fields", msg_unknown->field_count());

    return OK;
}

void HandleRpcRequest(const rpc::TContainerRequest &req,
                      std::shared_ptr<TClient> client) {
    rpc::TContainerResponse rsp;
    std::string cmd, arg, opt;
    std::vector<std::string> opts;
    TError error;

    client->StartRequest();

    RequestToString(req, cmd, arg, opts);
    for (auto &o: opts)
        opt += " " + o;

    bool silent = !Verbose && SilentRequest(req);
    if (!silent)
        L_REQ("{} {}{} from {}", cmd, arg, opt, client->Id);

    L_DBG("Raw request: {}", req.ShortDebugString());

    error = CheckRpcRequest(req);
    if (error)
        L_VERBOSE("Invalid request from {} : {} : {}", client->Id, error, req.ShortDebugString());
    else if (req.has_create())
        error = CreateContainer(req.create().name(), false);
    else if (req.has_createweak())
        error = CreateContainer(req.createweak().name(), true);
    else if (req.has_destroy())
        error = DestroyContainer(req.destroy());
    else if (req.has_list())
        error = ListContainers(req.list(), rsp);
    else if (req.has_getproperty())
        error = GetContainerProperty(req.getproperty(), rsp);
    else if (req.has_setproperty())
        error = SetContainerProperty(req.setproperty());
    else if (req.has_getdata())
        error = GetContainerData(req.getdata(), rsp);
    else if (req.has_get())
        error = GetContainerCombined(req.get(), rsp);
    else if (req.has_start())
        error = StartContainer(req.start());
    else if (req.has_stop())
        error = StopContainer(req.stop());
    else if (req.has_pause())
        error = PauseContainer(req.pause());
    else if (req.has_resume())
        error = ResumeContainer(req.resume());
    else if (req.has_respawn())
        error = RespawnContainer(req.respawn());
    else if (req.has_propertylist())
        error = ListProperty(rsp);
    else if (req.has_datalist())
        error = OK; // deprecated
    else if (req.has_kill())
        error = Kill(req.kill());
    else if (req.has_version())
        error = Version(rsp);
    else if (req.has_wait())
        error = Wait(req.wait(), rsp, client);
    else if (req.has_listvolumeproperties())
        error = ListVolumeProperties(rsp);
    else if (req.has_createvolume())
        error = CreateVolume(req.createvolume(), rsp);
    else if (req.has_linkvolume())
        error = LinkVolume(req.linkvolume());
    else if (req.has_linkvolumetarget())
        error = LinkVolume(req.linkvolumetarget());
    else if (req.has_unlinkvolume())
        error = UnlinkVolume(req.unlinkvolume());
    else if (req.has_listvolumes())
        error = ListVolumes(req.listvolumes(), rsp);
    else if (req.has_tunevolume())
        error = TuneVolume(req.tunevolume());
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
    else if (req.has_liststorage())
        error = ListStorage(req.liststorage(), rsp);
    else if (req.has_removestorage())
        error = RemoveStorage(req.removestorage());
    else if (req.has_importstorage())
        error = ImportStorage(req.importstorage());
    else if (req.has_exportstorage())
        error = ExportStorage(req.exportstorage());
    else if (req.has_locateprocess())
        error = LocateProcess(req.locateprocess(), rsp);
    else if (req.has_getsystem())
        error = GetSystemProperties(&req.getsystem(), rsp.mutable_getsystem());
    else if (req.has_setsystem())
        error = SetSystemProperties(&req.setsystem(), rsp.mutable_setsystem());
    else
        error = TError(EError::InvalidMethod, "invalid RPC method");

    client->FinishRequest();

    if (error != EError::Queued) {

        if (error) {
            Statistics->RequestsFailed++;
            switch (error.Error) {
                case EError::Unknown:
                    Statistics->FailSystem++;
                    break;
                case EError::InvalidValue:
                    Statistics->FailInvalidValue++;
                    break;
                case EError::InvalidCommand:
                    Statistics->FailInvalidCommand++;
                    break;
                default:
                    break;
            }
        }

        rsp.set_error(error.Error);
        rsp.set_errormsg(error.Message());

        /* log failed or slow silent requests */
        if (silent && (error || client->RequestTimeMs >= 1000)) {
            L_REQ("{} {}{} from {}", cmd, arg, opt, client->Id);
            silent = false;
        }

        if (!silent)
            L_RSP("{} {} {} to {} time={} ms", cmd, arg, ResponseAsString(rsp),
                    client->Id, client->RequestTimeMs);

        L_DBG("Raw response: {}", rsp.ShortDebugString());

        error = client->QueueResponse(rsp);
        if (error)
            L_WRN("Cannot send response for {} : {}", client->Id, error);
    }
}

void SendWaitResponse(TClient &client, const std::string &name) {
    rpc::TContainerResponse rsp;

    rsp.set_error(EError::Success);
    rsp.mutable_wait()->set_name(name);

    if (!name.empty() || Verbose)
        L_RSP("{} to {} (request took {} ms)", ResponseAsString(rsp),
                client.Id, client.RequestTimeMs);

    if (Debug)
        L_RSP("{} to {}", rsp.ShortDebugString(), client.Id);

    TError error = client.QueueResponse(rsp);
    if (error)
        L_WRN("Cannot send response for {} : {}", client.Id, error);
}
