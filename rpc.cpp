#include "rpc.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"

using std::string;

static TError CreateContainer(TContainerHolder &cholder,
                              const rpc::TContainerCreateRequest &req,
                              rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (container)
        return TError(EError::ContainerAlreadyExists, "invalid name");

    return cholder.Create(req.name());
}

static TError DestroyContainer(TContainerHolder &cholder,
                               const rpc::TContainerDestroyRequest &req,
                               rpc::TContainerResponse &rsp)
{
    return cholder.Destroy(req.name());
}

static TError StartContainer(TContainerHolder &cholder,
                             const rpc::TContainerStartRequest &req,
                             rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    return container->Start();
}

static TError StopContainer(TContainerHolder &cholder,
                            const rpc::TContainerStopRequest &req,
                            rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    return container->Stop();
}

static TError PauseContainer(TContainerHolder &cholder,
                             const rpc::TContainerPauseRequest &req,
                             rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    return container->Pause();
}

static TError ResumeContainer(TContainerHolder &cholder,
                              const rpc::TContainerResumeRequest &req,
                              rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    return container->Resume();
}

static TError ListContainers(TContainerHolder &cholder,
                             rpc::TContainerResponse &rsp)
{
    for (auto name : cholder.List())
        rsp.mutable_list()->add_name(name);

    return TError::Success();
}

static TError GetContainerProperty(TContainerHolder &cholder,
                                   const rpc::TContainerGetPropertyRequest &req,
                                   rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    string value;
    TError error(container->GetProperty(req.property(), value));
    if (!error)
        rsp.mutable_getproperty()->set_value(value);
    return error;
}

static TError SetContainerProperty(TContainerHolder &cholder,
                                   const rpc::TContainerSetPropertyRequest &req,
                                   rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    return container->SetProperty(req.property(), req.value());
}

static TError GetContainerData(TContainerHolder &cholder,
                               const rpc::TContainerGetDataRequest &req,
                               rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    if (dataSpec.find(req.data()) == dataSpec.end())
        return TError(EError::InvalidData, "invalid data");

    string value;
    TError error(container->GetData(req.data(), value));
    if (!error)
        rsp.mutable_getdata()->set_value(value);
    return error;
}

static TError ListProperty(TContainerHolder &cholder,
                           rpc::TContainerResponse &rsp)
{
    auto list = rsp.mutable_propertylist();

    for (auto kv : propertySpec) {
        if (kv.second.Flags & HIDDEN_PROPERTY)
            continue;

        auto entry = list->add_list();

        entry->set_name(kv.first);
        entry->set_desc(kv.second.Description);
    }

    return TError::Success();
}

static TError ListData(TContainerHolder &cholder,
                       rpc::TContainerResponse &rsp)
{
    auto list = rsp.mutable_datalist();

    for (auto kv : dataSpec) {
        auto entry = list->add_list();

        entry->set_name(kv.first);
        entry->set_desc(kv.second.Description);
    }

    return TError::Success();
}

static TError Kill(TContainerHolder &cholder,
                   const rpc::TContainerKillRequest &req,
                   rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container)
        return TError(EError::ContainerDoesNotExist, "invalid name");

    return container->Kill(req.sig());
}

rpc::TContainerResponse
HandleRpcRequest(TContainerHolder &cholder, const rpc::TContainerRequest &req)
{
    rpc::TContainerResponse rsp;
    string str;

    TLogger::LogRequest(req.ShortDebugString());

    rsp.set_error(EError::Unknown);

    TError error;
    try {
        if (req.has_create())
            error = CreateContainer(cholder, req.create(), rsp);
        else if (req.has_destroy())
            error = DestroyContainer(cholder, req.destroy(), rsp);
        else if (req.has_list())
            error = ListContainers(cholder, rsp);
        else if (req.has_getproperty())
            error = GetContainerProperty(cholder, req.getproperty(), rsp);
        else if (req.has_setproperty())
            error = SetContainerProperty(cholder, req.setproperty(), rsp);
        else if (req.has_getdata())
            error = GetContainerData(cholder, req.getdata(), rsp);
        else if (req.has_start())
            error = StartContainer(cholder, req.start(), rsp);
        else if (req.has_stop())
            error = StopContainer(cholder, req.stop(), rsp);
        else if (req.has_pause())
            error = PauseContainer(cholder, req.pause(), rsp);
        else if (req.has_resume())
            error = ResumeContainer(cholder, req.resume(), rsp);
        else if (req.has_propertylist())
            error = ListProperty(cholder, rsp);
        else if (req.has_datalist())
            error = ListData(cholder, rsp);
        else if (req.has_kill())
            error = Kill(cholder, req.kill(), rsp);
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
