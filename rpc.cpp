#include "log.hpp"
#include "rpc.hpp"
#include "protobuf.hpp"

extern "C" {
#include <sys/socket.h>
#include <sys/un.h>
}

using namespace std;

static void CreateContainer(TContainerHolder &cholder,
                            const rpc::TContainerCreateRequest &req,
                            rpc::TContainerResponse &rsp)
{
    try {
        auto c = cholder.Get(req.name());
        if (c) {
            rsp.set_error(rpc::EContainerError::ContainerAlreadyExists);
            return;
        }
        TError error = cholder.Create(req.name());
        if (!error)
            rsp.set_error(rpc::EContainerError::Success);
    } catch (...) {
        rsp.Clear();
        rsp.set_error(rpc::EContainerError::Error);
    }
}

static void DestroyContainer(TContainerHolder &cholder,
                             const rpc::TContainerDestroyRequest &req,
                             rpc::TContainerResponse &rsp)
{
    cholder.Destroy(req.name());
    rsp.set_error(rpc::EContainerError::Success);
}

static void StartContainer(TContainerHolder &cholder,
                           const rpc::TContainerStartRequest &req,
                           rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::ContainerDoesNotExist);
        return;
    }

    if (!container->Start())
        rsp.set_error(rpc::EContainerError::Success);
}

static void StopContainer(TContainerHolder &cholder,
                          const rpc::TContainerStopRequest &req,
                          rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::ContainerDoesNotExist);
        return;
    }

    if (!container->Stop())
        rsp.set_error(rpc::EContainerError::Success);
}

static void PauseContainer(TContainerHolder &cholder,
                           const rpc::TContainerPauseRequest &req,
                           rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::ContainerDoesNotExist);
        return;
    }

    if (!container->Pause())
        rsp.set_error(rpc::EContainerError::Success);
}

static void ResumeContainer(TContainerHolder &cholder,
                            const rpc::TContainerResumeRequest &req,
                            rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::ContainerDoesNotExist);
        return;
    }

    if (!container->Resume())
        rsp.set_error(rpc::EContainerError::Success);
}

static void ListContainers(TContainerHolder &cholder,
                           rpc::TContainerResponse &rsp)
{
    for (auto name : cholder.List())
        rsp.mutable_list()->add_name(name);

    rsp.set_error(rpc::EContainerError::Success);
}

static void GetContainerProperty(TContainerHolder &cholder,
                                 const rpc::TContainerGetPropertyRequest &req,
                                 rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::ContainerDoesNotExist);
        return;
    }

    if (propertySpec.find(req.property()) == propertySpec.end()) {
        rsp.set_error(rpc::EContainerError::InvalidProperty);
        return;
    }

    string value;
    container->GetProperty(req.property(), value);
    rsp.mutable_getproperty()->set_value(value);
    rsp.set_error(rpc::EContainerError::Success);
}

static void SetContainerProperty(TContainerHolder &cholder,
                                 const rpc::TContainerSetPropertyRequest &req,
                                 rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::ContainerDoesNotExist);
        return;
    }

    if (propertySpec.find(req.property()) == propertySpec.end()) {
        rsp.set_error(rpc::EContainerError::InvalidProperty);
        return;
    }

    TError error(container->SetProperty(req.property(), req.value()));
    if (!error)
        rsp.set_error(rpc::EContainerError::Success);
}

static void GetContainerData(TContainerHolder &cholder,
                             const rpc::TContainerGetDataRequest &req,
                             rpc::TContainerResponse &rsp)
{
    auto container = cholder.Get(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::ContainerDoesNotExist);
        return;
    }

    if (dataSpec.find(req.data()) == dataSpec.end()) {
        rsp.set_error(rpc::EContainerError::InvalidData);
        return;
    }

    string value;
    container->GetData(req.data(), value);
    rsp.mutable_getdata()->set_value(value);
    rsp.set_error(rpc::EContainerError::Success);
}

static void ListProperty(TContainerHolder &cholder,
                         rpc::TContainerResponse &rsp)
{
    auto list = rsp.mutable_propertylist();

    for (auto kv : propertySpec) {
        auto entry = list->add_list();

        entry->set_name(kv.first);
        entry->set_desc(kv.second.description);
    }

    rsp.set_error(rpc::EContainerError::Success);
}

static void ListData(TContainerHolder &cholder,
                     rpc::TContainerResponse &rsp)
{
    auto list = rsp.mutable_datalist();

    for (auto kv : dataSpec) {
        auto entry = list->add_list();

        entry->set_name(kv.first);
        entry->set_desc(kv.second.description);
    }

    rsp.set_error(rpc::EContainerError::Success);
}

rpc::TContainerResponse
HandleRpcRequest(TContainerHolder &cholder, const rpc::TContainerRequest &req)
{
    rpc::TContainerResponse rsp;
    string str;

    TLogger::LogRequest(req.ShortDebugString());

    rsp.set_error(rpc::EContainerError::Error);

    try {
        if (req.has_create())
            CreateContainer(cholder, req.create(), rsp);
        else if (req.has_destroy())
            DestroyContainer(cholder, req.destroy(), rsp);
        else if (req.has_list())
            ListContainers(cholder, rsp);
        else if (req.has_getproperty())
            GetContainerProperty(cholder, req.getproperty(), rsp);
        else if (req.has_setproperty())
            SetContainerProperty(cholder, req.setproperty(), rsp);
        else if (req.has_getdata())
            GetContainerData(cholder, req.getdata(), rsp);
        else if (req.has_start())
            StartContainer(cholder, req.start(), rsp);
        else if (req.has_stop())
            StopContainer(cholder, req.stop(), rsp);
        else if (req.has_pause())
            PauseContainer(cholder, req.pause(), rsp);
        else if (req.has_resume())
            ResumeContainer(cholder, req.resume(), rsp);
        else if (req.has_propertylist())
            ListProperty(cholder, rsp);
        else if (req.has_datalist())
            ListData(cholder, rsp);
        else
            rsp.set_error(rpc::EContainerError::InvalidMethod);
    } catch (...) {
        rsp.Clear();
        rsp.set_error(rpc::EContainerError::Error);
    }

    TLogger::LogResponse(rsp.ShortDebugString());

    return rsp;
}
