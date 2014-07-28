#include <iostream>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "rpc.pb.h"
#include "porto.h"

TContainerHolder cholder;

static rpc::TContainerResponse
CreateContainer(const rpc::TContainerCreateRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Create container: "<<req.name()<<std::endl;

    try {
        cholder.Create(req.name());
        rsp.set_error(rpc::EContainerError::Success);
    } catch (...) {
        rsp.set_error(rpc::EContainerError::AlreadyExists);
    }

    return rsp;
}

static rpc::TContainerResponse
DestroyContainer(const rpc::TContainerDestroyRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Destroy container: "<<req.name()<<std::endl;

    cholder.Destroy(req.name());
    rsp.set_error(rpc::EContainerError::Success);

    return rsp;
}

static rpc::TContainerResponse
StartContainer(const rpc::TContainerStartRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Start container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return rsp;
    }

    if (container->Start())
        rsp.set_error(rpc::EContainerError::Success);
    else
        rsp.set_error(rpc::EContainerError::Error);

    return rsp;
}

static rpc::TContainerResponse
StopContainer(const rpc::TContainerStopRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Stop container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return rsp;
    }

    if (container->Stop())
        rsp.set_error(rpc::EContainerError::Success);
    else
        rsp.set_error(rpc::EContainerError::Error);

    return rsp;
}

static rpc::TContainerResponse
PauseContainer(const rpc::TContainerPauseRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Pause container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return rsp;
    }

    if (container->Pause())
        rsp.set_error(rpc::EContainerError::Success);
    else
        rsp.set_error(rpc::EContainerError::Error);

    return rsp;
}

static rpc::TContainerResponse
ResumeContainer(const rpc::TContainerResumeRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Resume container: "<<req.name()<<std::endl;

    auto container = cholder.Find(req.name());
    if (!container) {
        rsp.set_error(rpc::EContainerError::DoesNotExist);
        return rsp;
    }

    if (container->Resume())
        rsp.set_error(rpc::EContainerError::Success);
    else
        rsp.set_error(rpc::EContainerError::Error);

    return rsp;
}

static rpc::TContainerResponse ListContainers()
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> List containers "<<std::endl;
    rsp.set_error(rpc::EContainerError::Success);

    for (auto name : cholder.List())
        rsp.mutable_list()->add_name(name);

    return rsp;
}

static rpc::TContainerResponse
HandleRequest(const rpc::TContainerRequest &req)
{
    if (req.has_create())
        return CreateContainer(req.create());

    else if (req.has_destroy())
        return DestroyContainer(req.destroy());

    else if (req.has_start())
        return StartContainer(req.start());

    else if (req.has_stop())
        return StopContainer(req.stop());

    else if (req.has_pause())
        return PauseContainer(req.pause());

    else if (req.has_resume())
        return ResumeContainer(req.resume());

    // TODO ...

    else if (req.has_list())
        return ListContainers();

    // TODO ...

    else {
        rpc::TContainerResponse rsp;
        rsp.set_error(rpc::EContainerError::InvalidMethod);
        return rsp;
    }
}

int main(int argc, char *argv[])
{
    rpc::TContainerRequest request;
    auto out = new google::protobuf::io::OstreamOutputStream(&std::cout);
    std::string str;

    for (std::string msg; std::getline(std::cin, msg);) {
        request.Clear();
        if (!google::protobuf::TextFormat::ParseFromString(msg, &request) ||
            !request.IsInitialized()) {
            std::cout<<"Skip invalid message:"<<std::endl;
            std::cout<<request.DebugString();
            std::cout<<std::endl;
            continue;
        }

        auto rsp = HandleRequest(request);
        google::protobuf::TextFormat::PrintToString(rsp, &str);
        std::cout<<str<<std::endl;
    }

    return 0;
}
