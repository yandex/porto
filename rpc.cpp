#include "rpc.hpp"
#include "property.hpp"
#include "data.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"

using std::string;

static TError CreateContainer(THolder &cholder,
                              const rpc::TContainerCreateRequest &req,
                              rpc::TContainerResponse &rsp,
                              const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (container)
        return TError(EError::ContainerAlreadyExists, "invalid name");

    return cholder.Create(req.name(), cred);
}

static TError DestroyContainer(THolder &cholder,
                               const rpc::TContainerDestroyRequest &req,
                               rpc::TContainerResponse &rsp,
                               const TCred &cred) {
    { // we don't want to hold container shared_ptr because Destroy
      // might think that it has some parent that holds it
        auto container = cholder.Get(req.name());
        if (container) {
            TError error = cholder.CheckPermission(container, cred);
            if (error)
                return error;
        }
    }

    return cholder.Destroy(req.name());
}

static TError StartContainer(THolder &cholder,
                             const rpc::TContainerStartRequest &req,
                             rpc::TContainerResponse &rsp,
                             const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    TError error = cholder.CheckPermission(container, cred);
    if (error)
        return error;

    return container->Start();
}

static TError StopContainer(THolder &cholder,
                            const rpc::TContainerStopRequest &req,
                            rpc::TContainerResponse &rsp,
                            const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    TError error = cholder.CheckPermission(container, cred);
    if (error)
        return error;

    return container->Stop();
}

static TError PauseContainer(THolder &cholder,
                             const rpc::TContainerPauseRequest &req,
                             rpc::TContainerResponse &rsp,
                             const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    TError error = cholder.CheckPermission(container, cred);
    if (error)
        return error;

    return container->Pause();
}

static TError ResumeContainer(THolder &cholder,
                              const rpc::TContainerResumeRequest &req,
                              rpc::TContainerResponse &rsp,
                              const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    TError error = cholder.CheckPermission(container, cred);
    if (error)
        return error;

    return container->Resume();
}

static TError ListContainers(THolder &cholder,
                             rpc::TContainerResponse &rsp) {
    for (auto name : cholder.List())
        rsp.mutable_list()->add_name(name);

    return TError::Success();
}

static TError GetContainerProperty(THolder &cholder,
                                   const rpc::TContainerGetPropertyRequest &req,
                                   rpc::TContainerResponse &rsp,
                                   const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    string value;
    TError error = container->GetProperty(req.property(), value);
    if (!error)
        rsp.mutable_getproperty()->set_value(value);
    return error;
}

static TError SetContainerProperty(THolder &cholder,
                                   const rpc::TContainerSetPropertyRequest &req,
                                   rpc::TContainerResponse &rsp,
                                   const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    TError error = cholder.CheckPermission(container, cred);
    if (error)
        return error;

    return container->SetProperty(req.property(), req.value(), cholder.PrivilegedUser(cred));
}

static TError GetContainerData(THolder &cholder,
                               const rpc::TContainerGetDataRequest &req,
                               rpc::TContainerResponse &rsp,
                               const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    string value;
    TError error = container->GetData(req.data(), value);
    if (!error)
        rsp.mutable_getdata()->set_value(value);
    return error;
}

static TError ListProperty(THolder &cholder,
                           rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_propertylist();

    for (auto property : propertySet.GetNames()) {
        auto p = propertySet.Get(property);
        if (p->Flags & HIDDEN_VALUE)
            continue;

        auto entry = list->add_list();

        entry->set_name(property);
        entry->set_desc(p->Desc);
    }

    return TError::Success();
}

static TError ListData(THolder &cholder,
                       rpc::TContainerResponse &rsp) {
    auto list = rsp.mutable_datalist();

    for (auto data : dataSet.GetNames()) {
        auto d = dataSet.Get(data);
        if (d->Flags & HIDDEN_VALUE)
            continue;

        auto entry = list->add_list();

        entry->set_name(data);
        entry->set_desc(d->Desc);
    }

    return TError::Success();
}

static TError Kill(THolder &cholder,
                   const rpc::TContainerKillRequest &req,
                   rpc::TContainerResponse &rsp,
                   const TCred &cred) {
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    TError error = cholder.CheckPermission(container, cred);
    if (error)
        return error;

    return container->Kill(req.sig());
}

static TError Version(THolder &cholder,
                      rpc::TContainerResponse &rsp) {
    auto ver = rsp.mutable_version();

    ver->set_tag(GIT_TAG);
    ver->set_revision(GIT_REVISION);

    return TError::Success();
}

rpc::TContainerResponse
HandleRpcRequest(THolder &cholder, const rpc::TContainerRequest &req,
                 const TCred &cred) {
    rpc::TContainerResponse rsp;
    string str;

    TLogger::LogRequest(req.ShortDebugString());

    rsp.set_error(EError::Unknown);

    TError error;
    try {
        if (req.has_create())
            error = CreateContainer(cholder, req.create(), rsp, cred);
        else if (req.has_destroy())
            error = DestroyContainer(cholder, req.destroy(), rsp, cred);
        else if (req.has_list())
            error = ListContainers(cholder, rsp);
        else if (req.has_getproperty())
            error = GetContainerProperty(cholder, req.getproperty(), rsp, cred);
        else if (req.has_setproperty())
            error = SetContainerProperty(cholder, req.setproperty(), rsp, cred);
        else if (req.has_getdata())
            error = GetContainerData(cholder, req.getdata(), rsp, cred);
        else if (req.has_start())
            error = StartContainer(cholder, req.start(), rsp, cred);
        else if (req.has_stop())
            error = StopContainer(cholder, req.stop(), rsp, cred);
        else if (req.has_pause())
            error = PauseContainer(cholder, req.pause(), rsp, cred);
        else if (req.has_resume())
            error = ResumeContainer(cholder, req.resume(), rsp, cred);
        else if (req.has_propertylist())
            error = ListProperty(cholder, rsp);
        else if (req.has_datalist())
            error = ListData(cholder, rsp);
        else if (req.has_kill())
            error = Kill(cholder, req.kill(), rsp, cred);
        else if (req.has_version())
            error = Version(cholder, rsp);
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

    rsp.set_error(error.GetError());
    rsp.set_errormsg(error.GetMsg());

    TLogger::LogResponse(rsp.ShortDebugString());

    return rsp;
}
