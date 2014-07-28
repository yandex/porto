#include <iostream>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "rpc.pb.h"

static rpc::TContainerResponse
CreateContainer(const rpc::TContainerCreateRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Create container: "<<req.name()<<std::endl;
    rsp.set_error(rpc::ContainerError::Success);

    return rsp;
}

static rpc::TContainerResponse ListContainers()
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> List containers "<<std::endl;
    rsp.set_error(rpc::ContainerError::Success);
    rsp.mutable_list()->add_name("test1");
    rsp.mutable_list()->add_name("test2");

    return rsp;
}

static rpc::TContainerResponse
HandleRequest(const rpc::TContainerRequest &req)
{
    if (req.has_create())
        return CreateContainer(req.create());

    // TODO ...

    else if (req.has_list())
        return ListContainers();

    // TODO ...

    else {
        rpc::TContainerResponse rsp;
        rsp.set_error(rpc::ContainerError::InvalidMethod);
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
