#include <fstream>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "rpc.pb.h"
#include "rpc.h"
#include "fdstream.h"

extern "C" {
#include <sys/socket.h>
#include <sys/un.h>
}

static void CreateContainer(TContainerHolder &cholder,
                            const rpc::TContainerCreateRequest &req,
                            rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Create container: "<<req.name()<<std::endl;

    try {
        cholder.Create(req.name());
        rsp.set_error(rpc::EContainerError::Success);
    } catch (...) {
        rsp.set_error(rpc::EContainerError::AlreadyExists);
    }
}

static void DestroyContainer(TContainerHolder &cholder,
                             const rpc::TContainerDestroyRequest &req,
                             rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Destroy container: "<<req.name()<<std::endl;

    cholder.Destroy(req.name());
    rsp.set_error(rpc::EContainerError::Success);
}

static void StartContainer(TContainerHolder &cholder,
                           const rpc::TContainerStartRequest &req,
                           rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Start container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return;
    }

    if (container->Start())
        rsp.set_error(rpc::EContainerError::Success);
}

static void StopContainer(TContainerHolder &cholder,
                          const rpc::TContainerStopRequest &req,
                          rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Stop container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return;
    }

    if (container->Stop())
        rsp.set_error(rpc::EContainerError::Success);
}

static void PauseContainer(TContainerHolder &cholder,
                           const rpc::TContainerPauseRequest &req,
                           rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Pause container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return;
    }

    if (container->Pause())
        rsp.set_error(rpc::EContainerError::Success);
}

static void ResumeContainer(TContainerHolder &cholder,
                            const rpc::TContainerResumeRequest &req,
                            rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Resume container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return;
    }

    if (container->Resume())
        rsp.set_error(rpc::EContainerError::Success);
}

static void ListContainers(TContainerHolder &cholder,
                           rpc::TContainerResponse &rsp)
{
    std::cout<<"-> List containers "<<std::endl;

    for (auto name : cholder.List())
        rsp.mutable_list()->add_name(name);

    rsp.set_error(rpc::EContainerError::Success);
}

static void GetContainerProperty(TContainerHolder &cholder,
                                 const rpc::TContainerGetPropertyRequest &req,
                                 rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Get "<<req.name()<<":"<<req.property()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return;
    }


#if 0
    auto val = container->GetProperty(req.property());
    rsp.getproperty.set_value(val);
    rsp.set_error(rpc::EContainerError::Success);
#endif
}

static void SetContainerProperty(TContainerHolder &cholder,
                                 const rpc::TContainerSetPropertyRequest &req,
                                 rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Set "<<req.name()<<":"<<req.property()<<"="<<req.value()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return;
    }


#if 0
    if (container->SetProperty(req.property(), req.value()))
        rsp.set_error(rpc::EContainerError::Success);
#endif
}

static void GetContainerData(TContainerHolder &cholder,
                             const rpc::TContainerGetDataRequest &req,
                             rpc::TContainerResponse &rsp)
{
    std::cout<<"-> Get data "<<req.name()<<":"<<req.data()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return;
    }


#if 0
    auto val = container->GetData(req.data());
    rsp.getproperty.set_value(val);
    rsp.set_error(rpc::EContainerError::Success);
#endif
}

static rpc::TContainerResponse
HandleRpcRequest(TContainerHolder &cholder, const rpc::TContainerRequest &req)
{
    rpc::TContainerResponse rsp;

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
        else
            rsp.set_error(rpc::EContainerError::InvalidMethod);
    } catch (...) {
        rsp.set_error(rpc::EContainerError::Error);
    }

    return rsp;
}

static string HandleRpcRequest(TContainerHolder &cholder, const string msg)
{
    string str;

    rpc::TContainerRequest request;
    if (!google::protobuf::TextFormat::ParseFromString(msg, &request) ||
        !request.IsInitialized())
        return "";

    auto rsp = HandleRpcRequest(cholder, request);
    google::protobuf::TextFormat::PrintToString(rsp, &str);

    return str;
}

int HandleRpcFromStream(TContainerHolder &cholder, istream &in, ostream &out)
{
    for (std::string msg; std::getline(in, msg);) {
        auto rsp = HandleRpcRequest(cholder, msg);
        if (rsp.length())
            out<<rsp<<std::endl;
        else
            out<<"Skip invalid message: "<<msg<<std::endl;
    }

    return 0;
}
