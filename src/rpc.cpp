#include <algorithm>

#include "rpc.hpp"
#include "client.hpp"
#include "config.hpp"
#include "version.hpp"
#include "property.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "waiter.hpp"
#include "event.hpp"
#include "helpers.hpp"
#include "util/thread.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "portod.hpp"
#include "storage.hpp"
#include "docker.hpp"
#include "util/quota.hpp"

#include <google/protobuf/descriptor.h>

extern "C" {
#include <unistd.h>
#include <sys/stat.h>
}

extern __thread char ReqId[9];

extern uint32_t RequestHandlingDelayMs;

void TRequest::Classify() {

    /* Normally not logged in non-verbose mode */
    RoReq =
        Req.has_version() ||
        Req.has_list() ||
        Req.has_findlabel() ||
        Req.has_listvolumes() ||
        Req.has_listlayers() ||
        Req.has_liststorage() ||
        Req.has_getlayerprivate() ||
        Req.has_get() ||
        Req.has_getdata() ||
        Req.has_getproperty() ||
        Req.has_datalist() ||
        Req.has_propertylist() ||
        Req.has_listvolumeproperties() ||
        Req.has_wait() ||
        Req.has_asyncwait() ||
        Req.has_stopasyncwait() ||
        Req.has_convertpath() ||
        Req.has_locateprocess() ||
        Req.has_getsystem() ||
        Req.has_listcontainersby() ||
        Req.has_getvolume();

    IoReq =
        Req.has_checkvolume() ||
        Req.has_importlayer() ||
        Req.has_exportlayer() ||
        Req.has_removelayer() ||
        Req.has_importstorage() ||
        Req.has_exportstorage() ||
        Req.has_removestorage() ||
        Req.has_createmetastorage() ||
        Req.has_removemetastorage() ||
        Req.has_dockerimagestatus() ||
        Req.has_listdockerimages() ||
        Req.has_pulldockerimage() ||
        Req.has_removedockerimage();

    VlReq =
        Req.has_createvolume() ||
        Req.has_tunevolume() ||
        Req.has_linkvolume() ||
        Req.has_unlinkvolume() ||
        Req.has_linkvolumetarget() ||
        Req.has_unlinkvolumetarget() ||
        Req.has_newvolume();

    SecretReq =
        (Req.has_setproperty() && StringStartsWith(Req.setproperty().property(), "env_secret")) ||
        (Req.has_createfromspec() && Req.createfromspec().has_container() && Req.createfromspec().container().has_env_secret()) ||
        (Req.has_updatefromspec() && Req.updatefromspec().has_container() && Req.updatefromspec().container().has_env_secret());
}

void TRequest::Parse() {
    std::vector<std::string> opts;

    Arg = "";

    if (Req.has_create()) {
        Cmd = "Create";
        Arg = Req.create().name();
    } else if (Req.has_createweak()) {
        Cmd = "Create";
        Arg = Req.createweak().name();
        opts = { "weak=true" };
    } else if (Req.has_destroy()) {
        Cmd = "Destroy";
        Arg = Req.destroy().name();
    } else if (Req.has_list()) {
        Cmd = "List";
        if (Req.list().has_mask())
            opts = { "mask=" + Req.list().mask() };
    } else if (Req.has_getproperty()) {
        Cmd = "Get";
        Arg = Req.getproperty().name();
        opts = { "property=" + Req.getproperty().property() };
        if (Req.getproperty().has_sync() && Req.getproperty().sync())
            opts.push_back("sync=true");
        if (Req.getproperty().has_real() && Req.getproperty().real())
            opts.push_back("real=true");
    } else if (Req.has_getdata()) {
        Cmd = "Get";
        Arg = Req.getdata().name();
        opts = { "property=" + Req.getdata().data() };
        if (Req.getdata().has_sync() && Req.getdata().sync())
            opts.push_back("sync=true");
        if (Req.getdata().has_real() && Req.getdata().real())
            opts.push_back("real=true");
    } else if (Req.has_get()) {
        Cmd = "Get";
        for (int i = 0; i < Req.get().name_size(); i++)
            opts.push_back(Req.get().name(i));
        opts.push_back("--");
        for (int i = 0; i < Req.get().variable_size(); i++)
            opts.push_back(Req.get().variable(i));
        if (Req.get().has_nonblock() && Req.get().nonblock())
            opts.push_back("nonblock=true");
        if (Req.get().has_sync() && Req.get().sync())
            opts.push_back("sync=true");
        if (Req.get().has_real() && Req.get().real())
            opts.push_back("real=true");
    } else if (Req.has_setproperty()) {
        Cmd = "Set";
        Arg = Req.setproperty().name();
        if (SecretReq)
            opts = { Req.setproperty().property() + "=<secret>" };
        else
            opts = { Req.setproperty().property() + "=" + Req.setproperty().value() };
    } else if (Req.has_start()) {
        Cmd = "Start";
        Arg = Req.start().name();
    } else if (Req.has_stop()) {
        Cmd = "Stop";
        Arg = Req.stop().name();
        if (Req.stop().has_timeout_ms())
            opts.push_back(fmt::format("timeout={} ms", Req.stop().timeout_ms()));
    } else if (Req.has_pause()) {
        Cmd = "Pause";
        Arg = Req.pause().name();
    } else if (Req.has_resume()) {
        Cmd = "Resume";
        Arg = Req.resume().name();
    } else if (Req.has_respawn()) {
        Cmd = "Respawn";
        Arg = Req.respawn().name();
    } else if (Req.has_wait()) {
        Cmd = "Wait";
        for (int i = 0; i < Req.wait().name_size(); i++)
            opts.push_back(Req.wait().name(i));
        if (Req.wait().has_timeout_ms())
            opts.push_back(fmt::format("timeout={} ms", Req.wait().timeout_ms()));
    } else if (Req.has_asyncwait()) {
        Cmd = "AsyncWait";
        for (int i = 0; i < Req.asyncwait().name_size(); i++)
            opts.push_back(Req.asyncwait().name(i));
        if (Req.asyncwait().has_timeout_ms())
            opts.push_back(fmt::format("timeout={} ms", Req.asyncwait().timeout_ms()));
    } else if (Req.has_propertylist() || Req.has_datalist()) {
        Cmd = "ListProperties";
    } else if (Req.has_kill()) {
        Cmd = "Kill";
        Arg = Req.kill().name();
        opts = { fmt::format("signal={}", Req.kill().sig()) };
    } else if (Req.has_version()) {
        Cmd = "Version";
    } else if (Req.has_createvolume()) {
        Cmd = "CreateVolume";
        Arg = Req.createvolume().path();
        for (auto p: Req.createvolume().properties())
            opts.push_back(p.name() + "=" + p.value());
    } else if (Req.has_linkvolume()) {
        Cmd = "LinkVolume";
        Arg = Req.linkvolume().path();
        opts = { "container=" + Req.linkvolume().container() };
    } else if (Req.has_linkvolumetarget()) {
        Cmd = "LinkVolume";
        Arg = Req.linkvolumetarget().path();
        opts = { "container=" + Req.linkvolumetarget().container() };
        if (Req.linkvolumetarget().target() != "")
            opts.push_back("target=" + Req.linkvolumetarget().target());
        if (Req.linkvolumetarget().read_only())
            opts.push_back("read_only=true");
        if (Req.linkvolumetarget().required())
            opts.push_back("Required=true");
    } else if (Req.has_unlinkvolume()) {
        Cmd = "UnlinkVolume";
        Arg = Req.unlinkvolume().path();
        opts = { "container=" + Req.unlinkvolume().container() };
        if (Req.unlinkvolume().has_target())
            opts.push_back("target=" + Req.unlinkvolume().target());
        if (Req.unlinkvolume().strict())
            opts.push_back("strict=true");
    } else if (Req.has_unlinkvolumetarget()) {
        Cmd = "UnlinkVolume";
        Arg = Req.unlinkvolumetarget().path();
        opts = { "container=" + Req.unlinkvolumetarget().container() };
        if (Req.unlinkvolumetarget().has_target())
            opts.push_back("target=" + Req.unlinkvolumetarget().target());
        if (Req.unlinkvolumetarget().strict())
            opts.push_back("strict=true");
    } else if (Req.has_listvolumes()) {
        Cmd = "ListVolumes";
        if (Req.listvolumes().has_path())
            Arg = Req.listvolumes().path();
        if (Req.listvolumes().has_container())
            opts = { "container=" + Req.listvolumes().container() };
    } else if (Req.has_tunevolume()) {
        Cmd = "TuneVolume";
        Arg = Req.tunevolume().path();
        for (auto p: Req.tunevolume().properties())
            opts.push_back(p.name() + "=" + p.value());
    } else if (Req.has_listvolumeproperties()) {
        Cmd = "ListVolumeProperities";
    } else if (Req.has_importlayer()) {
        Cmd = "ImportLayer";
        Arg = Req.importlayer().layer();
        opts = { "tarball=" + Req.importlayer().tarball() };
        if (Req.importlayer().has_compress())
            opts.push_back("compress=" + Req.importlayer().compress());
        if (Req.importlayer().has_place())
            opts.push_back("place=" + Req.importlayer().place());
        if (Req.importlayer().merge())
            opts.push_back("merge=true");
        if (Req.importlayer().has_private_value())
            opts.push_back("private=" + Req.importlayer().private_value());
        if (Req.importlayer().verbose_error())
            opts.push_back("verbose_error=true");
        if (Req.importlayer().has_container())
            opts.push_back("container=" + Req.importlayer().container());
    } else if (Req.has_exportlayer()) {
        if (Req.exportlayer().has_layer()) {
            Cmd = "ReexportLayer";
            Arg = Req.exportlayer().layer();
        } else {
            Cmd = "ExportLayer";
            Arg = Req.exportlayer().volume();
        }
        opts = { "tarball+" + Req.exportlayer().tarball() };
        if (Req.exportlayer().has_compress())
            opts.push_back("compress=" + Req.exportlayer().compress());
        if (Req.exportlayer().has_place())
            opts.push_back("place=" + Req.exportlayer().place());
    } else if (Req.has_removelayer()) {
        Cmd = "RemoveLayer";
        Arg = Req.removelayer().layer();
        if (Req.removelayer().has_place())
            opts.push_back("place=" + Req.removelayer().place());
    } else if (Req.has_listlayers()) {
        Cmd = "ListLayers";
        if (Req.listlayers().has_mask())
            opts.push_back("mask=" + Req.listlayers().mask());
        if (Req.listlayers().has_place())
            opts.push_back("place=" + Req.listlayers().place());
    } else if (Req.has_getlayerprivate()) {
        Cmd = "GetLayerPrivate";
    } else if (Req.has_setlayerprivate()) {
        Cmd = "SetLayerPrivate";
    } else if (Req.has_dockerimagestatus()) {
        Cmd = "DockerImageStatus";
        opts = { "name=" + Req.dockerimagestatus().name() };
        if (Req.dockerimagestatus().has_place())
            opts.push_back("place=" + Req.dockerimagestatus().place());
    } else if (Req.has_listdockerimages()) {
        Cmd = "ListDockerImages";
        if (Req.listdockerimages().has_mask())
            opts.push_back("mask=" + Req.listdockerimages().mask());
        if (Req.listdockerimages().has_place())
            opts.push_back("place=" + Req.listdockerimages().place());
    } else if (Req.has_pulldockerimage()) {
        Cmd = "PullDockerImage";
        opts = { "name=" + Req.pulldockerimage().name() };
        if (Req.pulldockerimage().has_place())
            opts.push_back("place=" + Req.pulldockerimage().place());
        if (Req.pulldockerimage().has_auth_token())
            opts.push_back("auth_token=***");
        if (Req.pulldockerimage().has_auth_host())
            opts.push_back("auth_host=" + Req.pulldockerimage().auth_host());
        if (Req.pulldockerimage().has_auth_service())
            opts.push_back("auth_service=" + Req.pulldockerimage().auth_service());
    } else if (Req.has_removedockerimage()) {
        Cmd = "RemoveDockerImage";
        opts = { "name=" + Req.removedockerimage().name() };
        if (Req.removedockerimage().has_place())
            opts.push_back("place=" + Req.removedockerimage().place());
    } else if (Req.has_convertpath()) {
        Cmd = "ConvertPath";
        Arg = Req.convertpath().path();
        opts = { "source=" + Req.convertpath().source(), "destination=" + Req.convertpath().destination() };
    } else if (Req.has_attachprocess()) {
        Cmd = "AttachProcess";
        Arg = Req.attachprocess().name();
        opts = { "pid=" + std::to_string(Req.attachprocess().pid()), "comm=" + Req.attachprocess().comm() };
    } else if (Req.has_attachthread()) {
        Cmd = "AttachThread";
        Arg = Req.attachthread().name();
        opts = { "pid=" + std::to_string(Req.attachthread().pid()), "comm=" + Req.attachthread().comm() };
    } else if (Req.has_locateprocess()) {
        Cmd = "LocateProcess";
        opts = { "pid=" + std::to_string(Req.locateprocess().pid()), "comm=" + Req.locateprocess().comm() };
    } else if (Req.has_findlabel()) {
        Cmd = "FindLabel";
        Arg = Req.findlabel().label();
        if (Req.findlabel().has_mask())
            opts.push_back("maks=" + Req.findlabel().mask());
        if (Req.findlabel().has_state())
            opts.push_back("state=" + Req.findlabel().state());
        if (Req.findlabel().has_value())
            opts.push_back("value=" + Req.findlabel().value());
    } else if (Req.has_setlabel()) {
        Cmd = "SetLabel";
        Arg = Req.setlabel().name();
        opts = { Req.setlabel().label() };
        if (Req.setlabel().has_value())
            opts.push_back("value=" + Req.setlabel().value());
        if (Req.setlabel().has_prev_value())
            opts.push_back("prev_value=" + Req.setlabel().prev_value());
    } else if (Req.has_inclabel()) {
        Cmd = "IncLabel";
        Arg = Req.inclabel().name();
        opts = { Req.inclabel().label() };
        if (Req.inclabel().has_add())
            opts.push_back(fmt::format("add={}", Req.inclabel().add()));
    } else if (Req.has_getsystem()) {
        Cmd = "GetSystem";
    } else if (Req.has_setsystem()) {
        Cmd = "SetSystem";
        Arg = Req.ShortDebugString();
    } else if (Req.has_clearstatistics()) {
        Cmd = "ClearStatistics";
        Arg = Req.ShortDebugString();
    } else if (Req.has_newvolume()) {
        Cmd = "NewVolume";
        Arg = Req.newvolume().volume().path();
    } else if (Req.has_createfromspec()) {
        Cmd = "CreateFromSpec";
        Arg = Req.createfromspec().container().name();
        if (SecretReq)
            Opt = "<secret>";
        else
            Opt = Req.createfromspec().ShortDebugString();
    } else if (Req.has_updatefromspec()) {
        Cmd = "UpdateFromSpec";
        Arg = Req.updatefromspec().container().name();
        if (SecretReq)
            Opt = "<secret>";
        else
            Opt = Req.updatefromspec().ShortDebugString();
    } else if (Req.has_listcontainersby()) {
        Cmd = "ListContainersBy";
    } else if (Req.has_getvolume()) {
        Cmd = "GetVolume";
    } else
        Cmd = "Unknown";

    for (auto &o: opts)
        Opt += " " + o;
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
            ret = "Wait " + resp.wait().name() + " state=" + resp.wait().state();
    } else if (resp.has_asyncwait()) {
        if (resp.asyncwait().name().empty())
            ret = "AsyncWait timeout";
        else
            ret = "AsyncWait " + resp.asyncwait().name() + " state=" + resp.asyncwait().state();
    } else if (resp.has_convertpath())
        ret = resp.convertpath().path();
    else
        ret = "Ok";

    return ret;
}

noinline TError CreateFromSpec(rpc::TCreateFromSpecRequest &req) {
    std::shared_ptr<TContainer> ct;
    std::string name;
    TError error;

    error = CL->ResolveName(req.container().name(), name);
    if (error)
        return error;

    error = TContainer::Create(name, ct);
    if (error)
        return error;

    ct->LockStateWrite();
    ct->IsWeak = true;
    ct->UnlockState();

    /* link volumes with container */
    for (auto volume_spec: *req.mutable_volumes()) {
        bool linked = false;
        for (auto &link: *volume_spec.mutable_links())
            linked |= link.container() == req.container().name();
        if (!linked) {
            auto link = volume_spec.add_links();
            link->set_container(req.container().name());
        }
    }

    error = CL->LockContainer(ct);
    if (error)
        return error;

    error = ct->Load(req.container());
    if (error)
        goto undo;

    CL->ReleaseContainer();

    for (auto volume_spec: req.volumes()) {
        std::shared_ptr<TVolume> volume;

        error = TVolume::Create(volume_spec, volume);
        if (error) {
            Statistics->VolumesFailed++;
            auto error2 = CL->LockContainer(ct);
            if (error2)
                return error2;
            goto undo;
        }
    }

    error = CL->LockContainer(ct);
    if (error)
        return error;

    if (req.container().weak()) {
        ct->SetProp(EProperty::WEAK);
        CL->WeakContainers.emplace_back(ct);
    } else
        ct->IsWeak = false;

    if (req.start()) {
        error = ct->Start();
        if (error)
            goto undo;
    }

    CL->ReleaseContainer();

    return OK;

undo:
    std::list<std::shared_ptr<TVolume>> unlinked;
    (void)ct->Destroy(unlinked);
    CL->ReleaseContainer();
    TVolume::DestroyUnlinked(unlinked);
    return error;
}

noinline TError UpdateFromSpec(const rpc::TUpdateFromSpecRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error;

    error = CL->WriteContainer(req.container().name(), ct);
    if (error)
        return error;

    error = ct->Load(req.container(), true);
    CL->ReleaseContainer();

    return error;
}

noinline TError ListContainersBy(const rpc::TListContainersRequest &req,
                             rpc::TListContainersResponse &rsp) {
    std::vector<std::string> names, props;
    std::unordered_map<std::string, std::string> propsOps;
    TError error;

    if (req.field_options().has_stdout_options()) {
        propsOps["stdout"] = fmt::format("{}:{}",
                                    req.field_options().stdout_options().stdstream_offset(),
                                    req.field_options().stdout_options().stdstream_limit());
    }

    if (req.field_options().has_stderr_options()) {
        propsOps["stderr"] = fmt::format("{}:{}",
                                    req.field_options().stderr_options().stdstream_offset(),
                                    req.field_options().stderr_options().stdstream_limit());
    }

    for(auto &prop: req.field_options().properties())
        props.push_back(prop);

    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        std::string name;
        if (CL->ComposeName(ct->Name, name))
            continue;
        names.push_back(name);
    }
    lock.unlock();

    std::set<std::string> found;

    for (const auto &name : names) {
        std::shared_ptr<TContainer> ct;
        lock.lock();
        auto error = CL->ResolveContainer(name, ct);
        lock.unlock();
        if (error)
            continue;
        for (auto filter : req.filters()) {
            if (StringMatch(name, filter.name())) {
                if (filter.has_labels() && !ct->MatchLabels(filter.labels()))
                    continue;

                found.insert(filter.name());
                auto container = rsp.add_containers();
                container->mutable_spec()->set_name(name);

                error = CL->LockContainer(ct);
                if (!error) {
                    ct->Dump(props, propsOps, *container);
                    CL->ReleaseContainer();
                } else
                    error.Dump(*container->mutable_error());
                break;
            }
        }
    }

    for (auto filter : req.filters()) {
        if (found.find(filter.name()) == found.end()) {
            auto container = rsp.add_containers();
            container->mutable_spec()->set_name(filter.name());
            error = TError(EError::ContainerDoesNotExist, "container " + filter.name() + " not found");
            error.Dump(*container->mutable_error());
        }
    }

    return OK;
}

static noinline TError CreateContainer(std::string reqName, bool weak) {
    std::string name;
    TError error = CL->ResolveName(reqName, name);
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
    std::list<std::shared_ptr<TVolume>> unlinked;
    std::shared_ptr<TContainer> ct;
    TError error;

    error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    error = ct->Destroy(unlinked);

    CL->ReleaseContainer();

    TVolume::DestroyUnlinked(unlinked);

    return error;
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
        if (req.has_changed_since() && ct->ChangeTime < req.changed_since())
            continue;
        rsp.mutable_list()->add_name(name);
    }
    return OK;
}

noinline TError FindLabel(const rpc::TFindLabelRequest &req, rpc::TFindLabelResponse &rsp) {
    auto label = req.label();
    bool wild_label = label.find_first_of("*?") != std::string::npos;

    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        std::string value;
        std::string name;

        if (!StringStartsWith(ct->Name, CL->PortoNamespace))
            continue;

        if (req.has_state() && TContainer::StateName(ct->State) != req.state())
            continue;

        name = ct->Name.substr(CL->PortoNamespace.length());
        if (req.has_mask() && !StringMatch(name, req.mask()))
            continue;

        if (wild_label) {
            for (auto &it: ct->Labels) {
                if (StringMatch(it.first, label) &&
                        (!req.has_value() || it.second == req.value())) {
                    auto l = rsp.add_list();
                    l->set_name(name);
                    l->set_state(TContainer::StateName(ct->State));
                    l->set_label(it.first);
                    l->set_value(it.second);
                }
            }
        } else if (!ct->GetLabel(label, value) &&
                (!req.has_value() || value == req.value())) {
            auto l = rsp.add_list();
            l->set_name(name);
            l->set_state(TContainer::StateName(ct->State));
            l->set_label(label);
            l->set_value(value);
        }
    }

    return OK;
}

noinline TError SetLabel(const rpc::TSetLabelRequest &req, rpc::TSetLabelResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error;

    error = TContainer::ValidLabel(req.label(), req.value());
    if (error)
        return error;

    error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    auto lock = LockContainers();
    auto it = ct->Labels.find(req.label());
    if (it != ct->Labels.end())
        rsp.set_prev_value(it->second);
    rsp.set_state(TContainer::StateName(ct->State));
    if (req.has_prev_value()) {
        if (it == ct->Labels.end()) {
            if (req.prev_value() != "")
                return TError(EError::LabelNotFound, "Container {} has no label {}", req.name(), req.label());
        } else if (req.prev_value() != it->second)
            return TError(EError::Busy, "Container {} label {} is {} not {}", req.name(), req.label(), it->second, req.prev_value());
    }
    if (req.has_state() && TContainer::StateName(ct->State) != req.state())
        return TError(EError::InvalidState, "Container {} is {} not {}", req.name(), rsp.state(), req.state());
    if (req.value() != "" && ct->Labels.size() >= PORTO_LABEL_COUNT_MAX)
        return TError(EError::ResourceNotAvailable, "Too many labels");
    ct->SetLabel(req.label(), req.value());
    lock.unlock();

    TContainerWaiter::ReportAll(*ct, req.label(), req.value());
    ct->Save();

    return OK;
}

noinline TError IncLabel(const rpc::TIncLabelRequest &req, rpc::TIncLabelResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    int64_t result = 0;
    TError error;

    error = TContainer::ValidLabel(req.label(), "");
    if (error)
        return error;

    error = CL->WriteContainer(req.name(), ct);
    if (error)
        return error;

    auto lock = LockContainers();
    error = ct->IncLabel(req.label(), result, req.add());
    rsp.set_result(result);
    lock.unlock();
    if (error)
        return error;

    TContainerWaiter::ReportAll(*ct, req.label(), std::to_string(result));
    ct->Save();

    return OK;
}

noinline TError GetContainerProperty(const rpc::TContainerGetPropertyRequest &req,
                                     rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (!error) {
        std::string value;

        ct->LockStateRead();

        if (req.has_real() && req.real()) {
            error = ct->HasProperty(req.property());
            if (error)
                goto out;
        }

        if (req.has_sync() && req.sync())
            ct->SyncProperty(req.property());

        error = ct->GetProperty(req.property(), value);
        if (!error)
            rsp.mutable_getproperty()->set_value(value);
out:
        ct->UnlockState();
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

    ct->LockStateWrite();
    error = ct->SetProperty(property, value);
    ct->UnlockState();

    return error;
}

noinline TError GetContainerData(const rpc::TContainerGetDataRequest &req,
                                 rpc::TContainerResponse &rsp) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->ReadContainer(req.name(), ct);
    if (!error) {
        std::string value;

        ct->LockStateRead();

        if (req.has_real() && req.real()) {
            error = ct->HasProperty(req.data());
            if (error)
                goto out;
        }

        if (req.has_sync() && req.sync())
            ct->SyncProperty(req.data());

        error = ct->GetProperty(req.data(), value);
        if (!error)
            rsp.mutable_getdata()->set_value(value);
out:
        ct->UnlockState();
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

    if (!containerError) {
        ct->LockStateRead();

        entry->set_change_time(ct->ChangeTime);

        if (req.has_changed_since() && ct->ChangeTime < req.changed_since()) {
            entry->set_no_changes(true);
            goto out;
        }
    }

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

out:
    if (!containerError)
        ct->UnlockState();
}

noinline TError GetContainerCombined(const rpc::TContainerGetRequest &req,
                                     rpc::TContainerResponse &rsp) {
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

    if (req.has_sync() && req.sync())
        TContainer::SyncPropertiesAll();

    for (auto &name: names)
        FillGetResponse(req, *get, name);

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

noinline TError ListDataProperty(rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_datalist();
    for (auto &elem : ContainerProperties) {
        auto &prop = elem.second;
        if (!prop->IsReadOnly || !prop->IsSupported || prop->IsHidden)
             continue;
         auto entry = list->add_list();
         entry->set_name(prop->Name);
         entry->set_desc(prop->GetDesc());
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
    ct->LockStateRead();
    error = ct->Kill(req.sig());
    ct->UnlockState();
    return error;
}

noinline TError Version(rpc::TContainerResponse &rsp) {
    auto ver = rsp.mutable_version();

    ver->set_tag(PORTO_VERSION);
    ver->set_revision(PORTO_REVISION);

    return OK;
}

noinline TError WaitContainers(const rpc::TContainerWaitRequest &req, bool async,
        rpc::TContainerResponse &rsp, std::shared_ptr<TClient> &client, bool stop = false) {
    std::string name, full_name;
    TError error;

    if (!req.name_size() && !async)
        return TError(EError::InvalidValue, "Containers to wait are not set");

    auto waiter = std::make_shared<TContainerWaiter>(async);
    if (req.has_target_state())
        waiter->TargetState = req.target_state();

    for (auto &label: req.label())
        waiter->Labels.push_back(label);

    auto lock = LockContainers();

    for (int i = 0; i < req.name_size(); i++) {
        name = req.name(i);

        if (name == "***") {
            waiter->Wildcards.push_back(name);
            continue;
        }

        error = client->ResolveName(name, full_name);
        if (error) {
            if (!stop)
                rsp.mutable_wait()->set_name(name);
            return error;
        }

        if (name.find_first_of("*?") != std::string::npos) {
            waiter->Wildcards.push_back(full_name);
            continue;
        }

        waiter->Names.push_back(full_name);
        if (stop)
            continue;

        std::shared_ptr<TContainer> ct;
        error = TContainer::Find(full_name, ct);
        if (error) {
            if (async)
                continue;
            rsp.mutable_wait()->set_name(name);
            return error;
        }

        if (!waiter->ShouldReport(*ct))
            continue;

        if (waiter->Labels.empty()) {
            client->MakeReport(name, TContainer::StateName(ct->State), async);
            if (!async)
                return TError::Queued();
        } else {
            for (auto &it: ct->Labels) {
                if (waiter->ShouldReportLabel(it.first)) {
                    client->MakeReport(name, TContainer::StateName(ct->State), async, it.first, it.second);
                    if (!async)
                        return TError::Queued();
                }
            }
        }
    }

    if (stop)
        return TContainerWaiter::Remove(*waiter, *client);

    if (!waiter->Wildcards.empty()) {
        for (auto &it: Containers) {
            auto &ct = it.second;
            if (!waiter->ShouldReport(*ct) || client->ComposeName(ct->Name, name))
                continue;

            if (waiter->Labels.empty()) {
                client->MakeReport(name, TContainer::StateName(ct->State), async);
                if (!async)
                    return TError::Queued();
            } else {
                for (auto &it: ct->Labels) {
                    if (waiter->ShouldReportLabel(it.first)) {
                        client->MakeReport(name, TContainer::StateName(ct->State), async, it.first, it.second);
                        if (!async)
                            return TError::Queued();
                    }
                }
            }
        }
    }

    if (req.has_timeout_ms() && req.timeout_ms() == 0) {
        client->MakeReport("", "timeout", async);
    } else {
        waiter->Activate(client);
        if (req.timeout_ms()) {
            TEvent e(EEventType::WaitTimeout, nullptr);
            e.WaitTimeout.Waiter = waiter;
            EventQueue->Add(req.timeout_ms(), e);
        }
    }

    return async ? OK : TError::Queued();
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

    TPath path(src->RootPath / req.path());
    path = dst->RootPath.InnerPath(path);

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

noinline TError CreateVolume(const rpc::TVolumeCreateRequest &req,
                             rpc::TContainerResponse &rsp) {
    std::shared_ptr<TVolume> volume;
    rpc::TVolumeSpec spec;
    TStringMap cfg;
    TError error;

    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    if (req.has_path() && !req.path().empty())
        cfg[V_PATH] = req.path();

    Statistics->VolumesCreated++;

    error = TVolume::VerifyConfig(cfg);
    if (error)
        goto err;

    error = TVolume::ParseConfig(cfg, spec);
    if (error)
        goto err;

    error = TVolume::Create(spec, volume);
    if (error)
        goto err;

    volume->DumpDescription(nullptr, volume->ComposePath(*CL->ClientContainer), rsp.mutable_volumedescription());
    return OK;

err:
    Statistics->VolumesFailed++;
    return error;
}

noinline TError TuneVolume(const rpc::TVolumeTuneRequest &req) {
    TStringMap cfg;
    for (auto p: req.properties())
        cfg[p.name()] = p.value();

    std::shared_ptr<TVolume> volume;
    TError error = CL->ControlVolume(req.path(), volume);
    if (error)
        return error;

    return volume->Tune(cfg);
}

noinline TError CheckVolume(const rpc::TVolumeCheckRequest &req) {
    std::shared_ptr<TVolume> volume;
    TError error = CL->ControlVolume(req.path(), volume);
    if (error)
        return error;
    return volume->Check();
}

noinline TError LinkVolume(const rpc::TVolumeLinkRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.container() != "" ? req.container() : SELF_CONTAINER, ct, true);
    if (error)
        return error;

    std::shared_ptr<TVolume> volume;
    error = CL->ControlVolume(req.path(), volume, !req.has_target() || req.read_only());
    if (error)
        return error;

    error = volume->LinkVolume(ct, req.target(), req.read_only(), req.required());
    if (error)
        return error;

    return OK;
}

noinline TError UnlinkVolume(const rpc::TVolumeUnlinkRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error;

    if (!req.has_container() || req.container() != "***") {
        error = CL->WriteContainer(req.has_container() ? req.container() :
                                    SELF_CONTAINER, ct, true);
        if (error)
            return error;
    }

    std::shared_ptr<TVolume> volume;
    error = CL->ControlVolume(req.path(), volume, true);
    if (error)
        return error;

    if (ct) {
        std::list<std::shared_ptr<TVolume>> unlinked;
        error = volume->UnlinkVolume(ct, req.has_target() ? req.target() : "***", unlinked, req.strict());
        CL->ReleaseContainer();
        TVolume::DestroyUnlinked(unlinked);
    } else {
        auto volumes_lock = LockVolumes();
        bool valid_for_removal = volume->State == EVolumeState::Ready;
        volumes_lock.unlock();

        error = valid_for_removal ? volume->Destroy() :
                TError(EError::VolumeNotReady, "Volume {} is not ready", req.path());
    }

    return error;
}

noinline TError ListVolumes(const rpc::TVolumeListRequest &req,
                            rpc::TContainerResponse &rsp) {
    TError error;

    if (req.has_path() && !req.path().empty()) {
        auto link = TVolume::ResolveLink(CL->ClientContainer->RootPath / req.path());
        if (!link)
            return TError(EError::VolumeNotFound, "Volume {} not found", req.path());
        auto entry = rsp.mutable_volumelist()->add_volumes();
        if (req.has_changed_since() && link->Volume->ChangeTime < req.changed_since()) {
            entry->set_path(req.path());
            entry->set_change_time(link->Volume->ChangeTime);
            entry->set_no_changes(true);
        } else
            link->Volume->DumpDescription(link.get(), req.path(), entry);
        return OK;
    }

    TPath base_path;
    if (req.has_container()) {
        std::shared_ptr<TContainer> ct;
        auto lock = LockContainers();
        error = CL->ResolveContainer(req.container(), ct);
        if (error)
            return error;
        base_path = ct->RootPath;
    } else
        base_path = CL->ClientContainer->RootPath;

    std::map<TPath, std::shared_ptr<TVolumeLink>> map;
    auto volumes_lock = LockVolumes();
    for (auto &it: VolumeLinks) {
        TPath path = base_path.InnerPath(it.first);
        if (path)
            map[path] = it.second;
    }
    volumes_lock.unlock();

    for (auto &it: map) {
        auto entry = rsp.mutable_volumelist()->add_volumes();
        auto volume = it.second->Volume.get();
        if (req.has_changed_since() && volume->ChangeTime < req.changed_since()) {
            entry->set_path(req.path());
            entry->set_change_time(volume->ChangeTime);
            entry->set_no_changes(true);
        } else
            volume->DumpDescription(it.second.get(), it.first, entry);
    }

    return OK;
}

noinline TError NewVolume(const rpc::TNewVolumeRequest &req,
                          rpc::TNewVolumeResponse &rsp) {
    Statistics->VolumesCreated++;

    std::shared_ptr<TVolume> volume;
    TError error = TVolume::Create(req.volume(), volume);
    if (error) {
        Statistics->VolumesFailed++;
        return error;
    }

    volume->Dump(*rsp.mutable_volume());
    return OK;
}

noinline TError GetVolume(const rpc::TGetVolumeRequest &req,
                          rpc::TGetVolumeResponse &rsp) {
    TError error;

    std::shared_ptr<TContainer> ct;
    std::string ct_name;

    if (req.has_container()) {
        auto lock = LockContainers();
        error = CL->ResolveContainer(req.container(), ct);
        if (error)
            return error;
        ct_name = req.container();
    } else {
        ct = CL->ClientContainer;
        ct_name = CL->RelativeName(CL->ClientContainer->Name);
    }

    for (auto &path: req.path()) {
        auto link = TVolume::ResolveLink(ct->RootPath / path);
        if (link) {
            auto volume = link->Volume.get();
            auto spec = rsp.add_volume();
            if (req.has_changed_since() && volume->ChangeTime < req.changed_since()) {
                spec->set_change_time(volume->ChangeTime);
                spec->set_no_changes(true);
            } else
                volume->Dump(*spec);
            spec->set_path(path);
            spec->set_container(ct_name);
        } else if (req.path().size() == 1)
            return TError(EError::VolumeNotFound, "Volume {} not found", path);
    }

    if (req.path().size() == 0) {
        std::map<TPath, std::shared_ptr<TVolumeLink>> map;

        auto volumes_lock = LockVolumes();
        for (auto &it: VolumeLinks) {
            TPath path = ct->RootPath.InnerPath(it.first);
            if (path)
                map[path] = it.second;
        }
        volumes_lock.unlock();

        for (auto &it: map) {
            auto volume = it.second->Volume.get();
            auto spec = rsp.add_volume();
            if (req.has_changed_since() && volume->ChangeTime < req.changed_since()) {
                spec->set_change_time(volume->ChangeTime);
                spec->set_no_changes(true);
            } else
                volume->Dump(*spec);
            spec->set_path(it.first.ToString());
            spec->set_container(ct_name);
        }
    }

    return OK;
}

noinline TError ImportLayer(const rpc::TLayerImportRequest &req) {
    TError error;
    TStorage layer;
    std::shared_ptr<TContainer> ct;
    std::string memCgroup = PORTO_HELPERS_CGROUP;

    if (req.has_container()) {
        error = CL->ReadContainer(req.container(), ct);
        if (error)
            return error;

        error = CL->CanControl(*ct);
        if (error)
            return error;

        memCgroup = ct->GetCgroup(MemorySubsystem).Name;
    }

    error = layer.Resolve(EStorageType::Layer, req.place(), req.layer());
    if (error)
        return error;

    if (req.has_private_value())
        layer.Private = req.private_value();

    layer.Owner = CL->Cred;

    return layer.ImportArchive(CL->ResolvePath(req.tarball()), memCgroup,
                               req.has_compress() ? req.compress() : "",
                               req.merge(),
                               req.verbose_error());
}

noinline TError GetLayerPrivate(const rpc::TLayerGetPrivateRequest &req,
                                rpc::TContainerResponse &rsp) {
    TStorage layer;
    TError error;

    error = layer.Resolve(EStorageType::Layer, req.place(), req.layer());
    if (error)
        return error;

    error = layer.Load();
    if (!error)
        rsp.mutable_layer_private()->set_private_value(layer.Private);
    return error;
}

noinline TError SetLayerPrivate(const rpc::TLayerSetPrivateRequest &req) {
    TStorage layer;
    TError error;

    error = layer.Resolve(EStorageType::Layer, req.place(), req.layer());
    if (error)
        return error;

    return layer.SetPrivate(req.private_value());
}

noinline TError ExportLayer(const rpc::TLayerExportRequest &req) {
    TStorage layer;
    TError error;

    if (req.has_layer()) {
        error = layer.Resolve(EStorageType::Layer, req.place(), req.layer(), true);
        if (error)
            return error;

        error = layer.Load();
        if (error)
            return error;

        return layer.ExportArchive(CL->ResolvePath(req.tarball()),
                                   req.has_compress() ? req.compress() : "");
    }

    std::shared_ptr<TVolume> volume;
    error = CL->ControlVolume(req.volume(), volume, true);
    if (error)
        return error;

    error = layer.Resolve(EStorageType::Volume, volume->Place, volume->Id);
    if (error)
        return error;

    layer.Owner = volume->VolumeOwner;
    error = volume->GetUpperLayer(layer.Path);
    if (error)
        return error;

    return layer.ExportArchive(CL->ResolvePath(req.tarball()),
                               req.has_compress() ? req.compress() : "");
}

noinline TError RemoveLayer(const rpc::TLayerRemoveRequest &req) {
    TStorage layer;
    TError error;
    bool async = req.has_async() ? req.async() : false;

    error = layer.Resolve(EStorageType::Layer, req.place(), req.layer());
    if (error)
        return error;

    return layer.Remove(false, async);
}

noinline TError ListLayers(const rpc::TLayerListRequest &req,
                           rpc::TContainerResponse &rsp) {
    TStorage place;
    TError error;

    error = place.Resolve(EStorageType::Place, req.place());
    if (error)
        return error;

    // porto layers
    std::list<TStorage> layers;
    error = place.List(EStorageType::Layer, layers);
    if (error)
        return error;

    // docker layers
    std::list<TStorage> dockerLayers;
    if (config().daemon().docker_images_support()) {
        error = place.List(EStorageType::DockerLayer, dockerLayers);
        if (error)
            return error;
    }

    auto list = rsp.mutable_layers();
    auto addLayer = [&](TStorage &layer) {
        if (req.has_mask() && !StringMatch(layer.Name, req.mask()))
            return;
        list->add_layer(layer.Name);
        (void)layer.Load();
        auto desc = list->add_layers();
        desc->set_name(layer.Name);
        desc->set_owner_user(layer.Owner.User());
        desc->set_owner_group(layer.Owner.Group());
        desc->set_private_value(layer.Private);
        desc->set_last_usage(layer.LastUsage());
        return;
    };
    std::for_each(layers.begin(), layers.end(), addLayer);
    std::for_each(dockerLayers.begin(), dockerLayers.end(), addLayer);

    return error;
}

noinline TError DockerImageStatus(const rpc::TDockerImageStatusRequest &req,
                                  rpc::TDockerImageStatusResponse &rsp) {
    TStorage place;
    TError error;

    if (!config().daemon().docker_images_support())
        return TError(EError::NotSupported, "Docker images are not supported");

    error = place.Resolve(EStorageType::Place, req.place());
    if (error)
        return error;

    TDockerImage image(req.name());
    error = image.Status(place.Place);
    if (error)
        return error;

    auto desc = rsp.mutable_image();
    desc->set_full_name(image.FullName());
    for (const auto &layer: image.Layers)
        desc->add_layers(layer.Digest);
    desc->set_command(MergeWithQuotes(image.Command, ' '));
    desc->set_env(MergeEscapeStrings(image.Env, ';'));

    return OK;
}

noinline TError ListDockerImages(const rpc::TDockerImageListRequest &req,
                                 rpc::TDockerImageListResponse &rsp) {
    TStorage place;
    TError error;

    if (!config().daemon().docker_images_support())
        return TError(EError::NotSupported, "Docker images are not supported");

    error = place.Resolve(EStorageType::Place, req.place());
    if (error)
        return error;

    std::vector<TDockerImage> images;
    error = TDockerImage::List(place.Place, images, req.has_mask() ? req.mask() : "");
    if (error)
        return error;

    for (const auto &image: images) {
        for (const auto &tag: image.Tags) {
            auto desc = rsp.add_images();
            desc->set_full_name(image.FullName(tag));
            for (const auto &layer: image.Layers)
                desc->add_layers(layer.Digest);
            desc->set_command(MergeWithQuotes(image.Command, ' '));
            desc->set_env(MergeEscapeStrings(image.Env, ';'));
        }
    }

    return OK;
}

noinline TError PullDockerImage(const rpc::TDockerImagePullRequest &req,
                                rpc::TDockerImagePullResponse &rsp) {
    TStorage place;
    TError error;

    if (!config().daemon().docker_images_support())
        return TError(EError::NotSupported, "Docker images are not supported");

    error = place.Resolve(EStorageType::Place, req.place());
    if (error)
        return error;

    TDockerImage image(req.name());

    if (!req.has_auth_token()) {
        if (req.has_auth_host())
            image.AuthHost = req.auth_host();
        if (req.has_auth_service())
            image.AuthService = req.auth_service();

        error = image.GetAuthToken();
        if (error)
            return error;
    } else
        image.AuthToken = req.auth_token();

    error = image.Download(place.Place);
    if (error)
        return error;

    auto desc = rsp.mutable_image();
    desc->set_full_name(image.FullName());
    for (const auto &layer: image.Layers)
        desc->add_layers(layer.Digest);
    desc->set_command(MergeWithQuotes(image.Command, ' '));
    desc->set_env(MergeEscapeStrings(image.Env, ';'));

    return OK;
}

noinline TError RemoveDockerImage(const rpc::TDockerImageRemoveRequest &req) {
    TStorage place;
    TError error;

    if (!config().daemon().docker_images_support())
        return TError(EError::NotSupported, "Docker images are not supported");

    error = place.Resolve(EStorageType::Place, req.place());
    if (error)
        return error;

    return TDockerImage(req.name()).Remove(place.Place);
}

noinline TError AttachProcess(const rpc::TAttachProcessRequest &req, bool thread) {
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

    auto lock = LockContainers();
    error = CL->ResolveContainer(req.name(), newCt);
    lock.unlock();
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

    L_ACT("Attach {} {} ({}) from {} to {}", thread ? "thread" : "process",
          pid, comm, oldCt->Name, newCt->Name);

    for (auto hy: Hierarchies) {
        auto cg = newCt->GetCgroup(*hy);
        error = cg.Attach(pid, thread);
        if (error)
            goto undo;
    }

    return OK;

undo:
    for (auto hy: Hierarchies) {
        auto cg = oldCt->GetCgroup(*hy);
        (void)cg.Attach(pid, thread);
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

    if (ct->IsRoot())
        name = ROOT_CONTAINER;
    else if (CL->ClientContainer == ct)
        name = SELF_CONTAINER;
    else
        name = CL->RelativeName(ct->Name);

    rsp.mutable_locateprocess()->set_name(name);

    return OK;
}

noinline TError ListStorage(const rpc::TStorageListRequest &req,
                            rpc::TContainerResponse &rsp) {
    TStorage place;
    TError error;

    error = place.Resolve(EStorageType::Place, req.place());
    if (error)
        return error;

    std::list<TStorage> storages;
    error = place.List(EStorageType::Storage, storages);
    if (error)
        return error;

    auto list = rsp.mutable_storagelist();
    for (auto &storage: storages) {
        if (req.has_mask() && !StringMatch(storage.Name, req.mask()))
            continue;
        if (storage.Load())
            continue;
        if (storage.Type == EStorageType::Storage) {
            auto desc = list->add_storages();
            desc->set_name(storage.Name);
            desc->set_owner_user(storage.Owner.User());
            desc->set_owner_group(storage.Owner.Group());
            desc->set_private_value(storage.Private);
            desc->set_last_usage(storage.LastUsage());
        }
        if (storage.Type == EStorageType::Meta) {
            auto desc = list->add_meta_storages();
            desc->set_name(storage.Name);
            desc->set_owner_user(storage.Owner.User());
            desc->set_owner_group(storage.Owner.Group());
            desc->set_private_value(storage.Private);
            desc->set_last_usage(storage.LastUsage());

            TProjectQuota quota(storage.Path);
            TStatFS stat;
            if (!quota.StatFS(stat)) {
                desc->set_space_limit(quota.SpaceLimit);
                desc->set_space_used(stat.SpaceUsage);
                desc->set_space_available(stat.SpaceAvail);
                desc->set_inode_limit(quota.InodeLimit);
                desc->set_inode_used(stat.InodeUsage);
                desc->set_inode_available(stat.InodeAvail);
            }
        }
    }

    return error;
}

noinline TError RemoveStorage(const rpc::TStorageRemoveRequest &req) {
    TStorage storage;
    TError error;

    error = storage.Resolve(EStorageType::Storage, req.place(), req.name());
    if (error)
        return error;

    return storage.Remove();
}

noinline TError ImportStorage(const rpc::TStorageImportRequest &req) {
    TStorage storage;
    TError error;

    error = storage.Resolve(EStorageType::Storage, req.place(), req.name());
    if (error)
        return error;

    storage.Owner = CL->Cred;
    if (req.has_private_value())
        storage.Private = req.private_value();

    return storage.ImportArchive(CL->ResolvePath(req.tarball()), PORTO_HELPERS_CGROUP,
                                 req.has_compress() ? req.compress() : "");
}

noinline TError ExportStorage(const rpc::TStorageExportRequest &req) {
    TStorage storage;
    TError error;

    error = storage.Resolve(EStorageType::Storage, req.place(), req.name(), true);
    if (error)
        return error;

    error = storage.Load();
    if (error)
        return error;

    return storage.ExportArchive(CL->ResolvePath(req.tarball()),
                                 req.has_compress() ? req.compress() : "");
}

noinline TError CreateMetaStorage(const rpc::TMetaStorage &req) {
    TStorage storage;
    TError error;

    error = storage.Resolve(EStorageType::Meta, req.place(), req.name());
    if (error)
        return error;

    storage.Owner = CL->Cred;
    if (req.has_private_value())
        storage.Private = req.private_value();

    return storage.CreateMeta(req.space_limit(), req.inode_limit());
}

noinline TError ResizeMetaStorage(const rpc::TMetaStorage &req) {
    TStorage storage;
    TError error;

    error = storage.Resolve(EStorageType::Meta, req.place(), req.name());
    if (error)
        return error;

    error = storage.Load();
    if (error)
        return error;

    if (req.has_private_value()) {
        error = storage.SetPrivate(req.private_value());
        if (error)
            return error;
    }

    if (req.has_space_limit() || req.has_inode_limit()) {
        error = storage.ResizeMeta(req.space_limit(), req.inode_limit());
        if (error)
            return error;
    }

    return OK;
}

noinline TError RemoveMetaStorage(const rpc::TMetaStorage &req) {
    TStorage storage;
    TError error;

    error = storage.Resolve(EStorageType::Meta, req.place(), req.name());
    if (error)
        return error;

    return storage.Remove();
}

noinline TError SetSymlink(const rpc::TSetSymlinkRequest &req) {
    std::shared_ptr<TContainer> ct;
    TError error = CL->WriteContainer(req.container(), ct);
    if (error)
        return error;
    error = ct->SetSymlink(req.symlink(), req.target());
    if (error)
        return error;
    return ct->Save();
}

noinline static TError GetSystemProperties(const rpc::TGetSystemRequest *, rpc::TGetSystemResponse *rsp) {
    rsp->set_porto_version(PORTO_VERSION);
    rsp->set_porto_revision(PORTO_REVISION);
    rsp->set_kernel_version(config().linux_version());
    rsp->set_errors(Statistics->Errors);
    rsp->set_warnings(Statistics->Warns);
    rsp->set_fatals(Statistics->Fatals);
    rsp->set_porto_starts(Statistics->PortoStarts);
    rsp->set_porto_uptime((GetCurrentTimeMs() - Statistics->PortoStarted) / 1000);
    rsp->set_master_uptime((GetCurrentTimeMs() - Statistics->MasterStarted) / 1000);
    rsp->set_taints(Statistics->Taints);

    rsp->set_verbose(Verbose);
    rsp->set_debug(Debug);
    rsp->set_log_lines(Statistics->LogLines);
    rsp->set_log_bytes(Statistics->LogBytes);
    rsp->set_log_lines_lost(Statistics->LogLinesLost);
    rsp->set_log_bytes_lost(Statistics->LogBytesLost);
    rsp->set_log_open(Statistics->LogOpen);

    rsp->set_container_count(Statistics->ContainersCount - NR_SERVICE_CONTAINERS);
    rsp->set_container_limit(config().container().max_total());
    rsp->set_container_running(RootContainer->RunningChildren);
    rsp->set_container_created(Statistics->ContainersCreated);
    rsp->set_container_started(Statistics->ContainersStarted);
    rsp->set_container_start_failed(Statistics->ContainersFailedStart);
    rsp->set_container_oom(Statistics->ContainersOOM);
    rsp->set_container_buried(Statistics->RemoveDead);
    rsp->set_container_lost(Statistics->ContainerLost);
    rsp->set_container_tainted(Statistics->ContainersTainted);
    rsp->set_postfork_issues(Statistics->PostForkIssues);

    rsp->set_stream_rotate_bytes(Statistics->LogRotateBytes);
    rsp->set_stream_rotate_errors(Statistics->LogRotateErrors);

    rsp->set_volume_count(Statistics->VolumesCount);
    rsp->set_volume_limit(config().volumes().max_total());
    rsp->set_volume_created(Statistics->VolumesCreated);
    rsp->set_volume_failed(Statistics->VolumesFailed);
    rsp->set_volume_links(Statistics->VolumeLinks);
    rsp->set_volume_links_mounted(Statistics->VolumeLinksMounted);
    rsp->set_volume_lost(Statistics->VolumeLost);

    rsp->set_layer_import(Statistics->LayerImport);
    rsp->set_layer_export(Statistics->LayerExport);
    rsp->set_layer_remove(Statistics->LayerRemove);

    rsp->set_client_count(Statistics->ClientsCount);
    rsp->set_client_max(config().daemon().max_clients());
    rsp->set_client_connected(Statistics->ClientsConnected);

    rsp->set_request_queued(Statistics->RequestsQueued);
    rsp->set_request_completed(Statistics->RequestsCompleted);
    rsp->set_request_failed(Statistics->RequestsFailed);
    rsp->set_request_threads(config().daemon().rw_threads());
    rsp->set_request_longer_1s(Statistics->RequestsLonger1s);
    rsp->set_request_longer_3s(Statistics->RequestsLonger3s);
    rsp->set_request_longer_30s(Statistics->RequestsLonger30s);
    rsp->set_request_longer_5m(Statistics->RequestsLonger5m);
    rsp->set_request_top_running_time(RpcRequestsTopRunningTime() / 1000);

    rsp->set_fail_system(Statistics->FailSystem);
    rsp->set_fail_invalid_value(Statistics->FailInvalidValue);
    rsp->set_fail_invalid_command(Statistics->FailInvalidCommand);
    rsp->set_fail_memory_guarantee(Statistics->FailMemoryGuarantee);
    rsp->set_fail_invalid_netaddr(Statistics->FailInvalidNetaddr);

    rsp->set_network_count(Statistics->NetworksCount);
    rsp->set_network_created(Statistics->NetworksCreated);
    rsp->set_network_problems(Statistics->NetworkProblems);
    rsp->set_network_repairs(Statistics->NetworkRepairs);

    return OK;
}

noinline static TError SetSystemProperties(const rpc::TSetSystemRequest *req, rpc::TSetSystemResponse *) {
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

    if (req->has_frozen()) {
        PortodFrozen = req->frozen();
        L_SYS("{} porto", PortodFrozen ? "Freeze" : "Unfreeze");
    }

    return OK;
}

noinline static TError ClearStatistics(const rpc::TClearStatisticsRequest *req) {
    if (req->has_stat()) {
        auto it = PortoStatMembers.find(req->stat());

        if (it == PortoStatMembers.end())
            return TError(EError::InvalidValue, "Unknown statistic");

         if (!it->second.Resetable)
            return TError(EError::InvalidValue, "Field cannot be cleared");

        Statistics->*(it->second.Member) = 0;
    } else {
        for (const auto &it : PortoStatMembers) {
            if (it.second.Resetable)
                Statistics->*(it.second.Member) = 0;
        }
    }

    return OK;
}

TError TRequest::Check() {
    auto req_ref = Req.GetReflection();

    std::vector<const google::protobuf::FieldDescriptor *> req_fields;
    req_ref->ListFields(Req, &req_fields);

    if (req_fields.size() != 1)
        return TError(EError::InvalidMethod, "Request has {} known methods", req_fields.size());

    auto msg = &req_ref->GetMessage(Req, req_fields[0]);
    auto msg_ref = msg->GetReflection();
    auto msg_unknown = &msg_ref->GetUnknownFields(*msg);

    if (msg_unknown->field_count() != 0)
        return TError(EError::InvalidMethod, "Request has {} unknown fields", msg_unknown->field_count());

    if (Cmd == "Unknown")
        Cmd = req_fields[0]->name();

    return OK;
}

void TRequest::Handle() {
    rpc::TContainerResponse rsp;
    TError error;

    Client->StartRequest();

    if (RequestHandlingDelayMs)
        usleep(RequestHandlingDelayMs * 1000);

    StartTime = GetCurrentTimeMs();
    auto timestamp = time(nullptr);
    bool specRequest = false;

    Parse();
    error = Check();

    if (!error && (!RoReq || Verbose))
        L_REQ("{} {} {} from {}", Cmd, Arg, Opt, Client->Id);

    if (Debug && !SecretReq)
        L_DBG("Raw request: {}", Req.ShortDebugString());

    if (error && Verbose)
        L_VERBOSE("Invalid request from {} : {} : {}", Client->Id, error, Req.ShortDebugString());
    else if (!RoReq && Client->AccessLevel <= EAccessLevel::ReadOnly)
        error = TError(EError::Permission, "Write access denied");
    else if (!RoReq && PortodFrozen && !Client->IsSuperUser())
        error = TError(EError::PortoFrozen, "Porto frozen, only root user might change anything");
    else if (Req.has_createfromspec()) {
        error = CreateFromSpec(*Req.mutable_createfromspec());
        specRequest = true;
    }
    else if (Req.has_updatefromspec()) {
        error = UpdateFromSpec(Req.updatefromspec());
        specRequest = true;
    }
    else if (Req.has_listcontainersby()) {
        error = ListContainersBy(Req.listcontainersby(), *rsp.mutable_listcontainersby());
        specRequest = true;
    }
    else if (Req.has_create())
        error = CreateContainer(Req.create().name(), false);
    else if (Req.has_createweak())
        error = CreateContainer(Req.createweak().name(), true);
    else if (Req.has_destroy())
        error = DestroyContainer(Req.destroy());
    else if (Req.has_list())
        error = ListContainers(Req.list(), rsp);
    else if (Req.has_getproperty())
        error = GetContainerProperty(Req.getproperty(), rsp);
    else if (Req.has_setproperty())
        error = SetContainerProperty(Req.setproperty());
    else if (Req.has_getdata())
        error = GetContainerData(Req.getdata(), rsp);
    else if (Req.has_get())
        error = GetContainerCombined(Req.get(), rsp);
    else if (Req.has_start())
        error = StartContainer(Req.start());
    else if (Req.has_stop())
        error = StopContainer(Req.stop());
    else if (Req.has_pause())
        error = PauseContainer(Req.pause());
    else if (Req.has_resume())
        error = ResumeContainer(Req.resume());
    else if (Req.has_respawn())
        error = RespawnContainer(Req.respawn());
    else if (Req.has_propertylist())
        error = ListProperty(rsp);
    else if (Req.has_datalist())
        error = ListDataProperty(rsp); // deprecated
    else if (Req.has_kill())
        error = Kill(Req.kill());
    else if (Req.has_version())
        error = Version(rsp);
    else if (Req.has_wait())
        error = WaitContainers(Req.wait(), false, rsp, Client);
    else if (Req.has_asyncwait())
        error = WaitContainers(Req.asyncwait(), true, rsp, Client);
    else if (Req.has_stopasyncwait())
        error = WaitContainers(Req.stopasyncwait(), true, rsp, Client, true);
    else if (Req.has_listvolumeproperties())
        error = ListVolumeProperties(rsp);
    else if (Req.has_createvolume())
        error = CreateVolume(Req.createvolume(), rsp);
    else if (Req.has_linkvolume())
        error = LinkVolume(Req.linkvolume());
    else if (Req.has_linkvolumetarget())
        error = LinkVolume(Req.linkvolumetarget());
    else if (Req.has_unlinkvolume())
        error = UnlinkVolume(Req.unlinkvolume());
    else if (Req.has_unlinkvolumetarget())
        error = UnlinkVolume(Req.unlinkvolumetarget());
    else if (Req.has_listvolumes())
        error = ListVolumes(Req.listvolumes(), rsp);
    else if (Req.has_tunevolume())
        error = TuneVolume(Req.tunevolume());
    else if (Req.has_checkvolume())
        error = CheckVolume(Req.checkvolume());
    else if (Req.has_newvolume())
        error = NewVolume(Req.newvolume(), *rsp.mutable_newvolume());
    else if (Req.has_getvolume())
        error = GetVolume(Req.getvolume(), *rsp.mutable_getvolume());
    else if (Req.has_importlayer())
        error = ImportLayer(Req.importlayer());
    else if (Req.has_exportlayer())
        error = ExportLayer(Req.exportlayer());
    else if (Req.has_removelayer())
        error = RemoveLayer(Req.removelayer());
    else if (Req.has_listlayers())
        error = ListLayers(Req.listlayers(), rsp);
    else if (Req.has_dockerimagestatus())
        error = DockerImageStatus(Req.dockerimagestatus(), *rsp.mutable_dockerimagestatus());
    else if (Req.has_listdockerimages())
        error = ListDockerImages(Req.listdockerimages(), *rsp.mutable_listdockerimages());
    else if (Req.has_pulldockerimage())
        error = PullDockerImage(Req.pulldockerimage(), *rsp.mutable_pulldockerimage());
    else if (Req.has_removedockerimage())
        error = RemoveDockerImage(Req.removedockerimage());
    else if (Req.has_convertpath())
        error = ConvertPath(Req.convertpath(), rsp);
    else if (Req.has_attachprocess())
        error = AttachProcess(Req.attachprocess(), false);
    else if (Req.has_attachthread())
        error = AttachProcess(Req.attachthread(), true);
    else if (Req.has_getlayerprivate())
        error = GetLayerPrivate(Req.getlayerprivate(), rsp);
    else if (Req.has_setlayerprivate())
        error = SetLayerPrivate(Req.setlayerprivate());
    else if (Req.has_liststorage())
        error = ListStorage(Req.liststorage(), rsp);
    else if (Req.has_removestorage())
        error = RemoveStorage(Req.removestorage());
    else if (Req.has_importstorage())
        error = ImportStorage(Req.importstorage());
    else if (Req.has_exportstorage())
        error = ExportStorage(Req.exportstorage());
    else if (Req.has_createmetastorage())
        error = CreateMetaStorage(Req.createmetastorage());
    else if (Req.has_resizemetastorage())
        error = ResizeMetaStorage(Req.resizemetastorage());
    else if (Req.has_removemetastorage())
        error = RemoveMetaStorage(Req.removemetastorage());
    else if (Req.has_setsymlink())
        error = SetSymlink(Req.setsymlink());
    else if (Req.has_locateprocess())
        error = LocateProcess(Req.locateprocess(), rsp);
    else if (Req.has_findlabel())
        error = FindLabel(Req.findlabel(), *rsp.mutable_findlabel());
    else if (Req.has_setlabel())
        error = SetLabel(Req.setlabel(), *rsp.mutable_setlabel());
    else if (Req.has_inclabel())
        error = IncLabel(Req.inclabel(), *rsp.mutable_inclabel());
    else if (Req.has_getsystem())
        error = GetSystemProperties(&Req.getsystem(), rsp.mutable_getsystem());
    else if (Req.has_setsystem())
        error = SetSystemProperties(&Req.setsystem(), rsp.mutable_setsystem());
    else if (Req.has_clearstatistics())
        error = ClearStatistics(&Req.clearstatistics());
    else
        error = TError(EError::InvalidMethod, "invalid RPC method");

    FinishTime = GetCurrentTimeMs();
    Client->FinishRequest();

    uint64_t RequestTime = FinishTime - QueueTime;

    Statistics->RequestsQueued--;
    if (!specRequest) {
        Statistics->RequestsCompleted++;

        if (RequestTime > 1000)
            Statistics->RequestsLonger1s++;
        if (RequestTime > 3000)
            Statistics->RequestsLonger3s++;
        if (RequestTime > 30000)
            Statistics->RequestsLonger30s++;
        if (RequestTime > 300000)
            Statistics->RequestsLonger5m++;

        if (RoReq && RequestTime > Statistics->LongestRoRequest) {
            L("Longest read request {} time={}+{} ms", Cmd,
                    StartTime - QueueTime, FinishTime - StartTime);
            Statistics->LongestRoRequest = RequestTime;
        }
    } else {
        Statistics->SpecRequestsCompleted++;

        if (RequestTime > 1000)
            Statistics->SpecRequestsLonger1s++;
        if (RequestTime > 3000)
            Statistics->SpecRequestsLonger3s++;
        if (RequestTime > 30000)
            Statistics->SpecRequestsLonger30s++;
        if (RequestTime > 300000)
            Statistics->SpecRequestsLonger5m++;
    }

    if (error == EError::Queued)
        goto exit;

    if (error) {
        if (!rsp.IsInitialized())
            rsp.Clear();
        if (!specRequest)
            Statistics->RequestsFailed++;
        else {
            Statistics->SpecRequestsFailed++;
            if (error == EError::Unknown)
                Statistics->SpecRequestsFailedUnknown++;
            else if (error == EError::InvalidValue)
                Statistics->SpecRequestsFailedInvalidValue++;
            else if (error == EError::ContainerDoesNotExist)
                Statistics->SpecRequestsFailedContainerDoesNotExist++;
        }
        AccountErrorType(error);
    }

    rsp.set_error(error.Error);
    rsp.set_errormsg(error.Message());
    rsp.set_timestamp(timestamp);

    if (!RoReq || Verbose) {
        L_RSP("{} {} {} to {} time={}+{} ms", Cmd, Arg, ResponseAsString(rsp),
                Client->Id, StartTime - QueueTime, FinishTime - StartTime);
    } else if (error || RequestTime >= 1000) {
        /* Log failed or slow silent requests without details */
        L_REQ("{} {} from {}", Cmd, Arg, Client->Id);
        L_RSP("{} {} {} to {} time={}+{} ms", Cmd, Arg, error,
                Client->Id, StartTime - QueueTime, FinishTime - StartTime);
    }

    if (Debug)
        L_DBG("Raw response: {}", rsp.ShortDebugString());

exit:
    auto lock = Client->Lock();

    if (error != EError::Queued) {
        Client->Processing = false;
        error = Client->QueueResponse(rsp);
        if (!error && !Client->Sending)
            error = Client->SendResponse(true);
        if (error)
            L_WRN("Cannot send response for {} : {}", Client->Id, error);
    }

    if (Client->CloseAfterResponse)
        Client->CloseConnectionLocked();
}

void TRequest::ChangeId() {
    snprintf(ReqId, sizeof(ReqId), "%.8x", rand());
}

class TRequestQueue {
    std::vector<std::unique_ptr<std::thread>> Threads;
    std::vector<uint64_t> StartTime;
    std::queue<std::unique_ptr<TRequest>> Queue;
    std::condition_variable Wakeup;
    std::mutex Mutex;
    bool ShouldStop = false;
    const std::string Name;

public:
    TRequestQueue(const std::string &name) : Name(name) {}

    void Start(int thread_count) {
        StartTime.resize(thread_count);
        for (int index = 0; index < thread_count; index++)
            Threads.emplace_back(NewThread(&TRequestQueue::Run, this, index));
    }

    void Stop() {
        Mutex.lock();
        ShouldStop = true;
        Mutex.unlock();
        Wakeup.notify_all();
        for (auto &thread: Threads)
            thread->join();
        Threads.clear();
        ShouldStop = false;
    }

    void Enqueue(std::unique_ptr<TRequest> &request) {
        Mutex.lock();
        Queue.push(std::move(request));
        Mutex.unlock();
        Wakeup.notify_one();
    }

    void Run(int index) {
        SetProcessName(fmt::format("{}{}", Name, index));
        srand(GetCurrentTimeMs());
        auto lock = std::unique_lock<std::mutex>(Mutex);
        while (true) {
            while (Queue.empty() && !ShouldStop)
                Wakeup.wait(lock);
            if (ShouldStop)
                break;
            auto request = std::unique_ptr<TRequest>(std::move(Queue.front()));
            Queue.pop();
            StartTime[index] = GetCurrentTimeMs();
            lock.unlock();
            request->ChangeId();
            request->Handle();
            request = nullptr;
            lock.lock();
            StartTime[index] = 0;
        }
        lock.unlock();
    }

    uint64_t TopRunningTime() {
        uint64_t now = GetCurrentTimeMs();
        uint64_t ret = 0;
        auto lock = std::unique_lock<std::mutex>(Mutex);
        for (auto start: StartTime) {
            if (start)
                ret = std::max(ret, now - start);
        }
        return ret;
    }
};

static TRequestQueue RwQueue("portod-RW");
static TRequestQueue RoQueue("portod-RO");
static TRequestQueue IoQueue("portod-IO");
static TRequestQueue VlQueue("portod-VL"); // queue for volume operations

void StartRpcQueue() {
    RwQueue.Start(config().daemon().rw_threads());
    RoQueue.Start(config().daemon().ro_threads());
    IoQueue.Start(config().daemon().io_threads());
    VlQueue.Start(config().daemon().vl_threads());
}

void StopRpcQueue() {
    RwQueue.Stop();
    RoQueue.Stop();
    IoQueue.Stop();
    VlQueue.Stop();
}

void QueueRpcRequest(std::unique_ptr<TRequest> &request) {
    Statistics->RequestsQueued++;
    request->QueueTime = GetCurrentTimeMs();
    request->Classify();
    if (request->RoReq)
        RoQueue.Enqueue(request);
    else if (request->IoReq)
        IoQueue.Enqueue(request);
    else if (request->VlReq)
        VlQueue.Enqueue(request);
    else
        RwQueue.Enqueue(request);
}


uint64_t RpcRequestsTopRunningTime() {
    auto rw = RwQueue.TopRunningTime();
    auto ro = RoQueue.TopRunningTime();
    auto io = IoQueue.TopRunningTime();
    auto vl = VlQueue.TopRunningTime();

    return std::max({rw, ro, io, vl});
}
