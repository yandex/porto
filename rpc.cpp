#include <iostream>
#include <fstream>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <ext/stdio_filebuf.h>

#include "rpc.pb.h"
#include "rpc.h"

extern "C" {
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
}

static rpc::TContainerResponse
CreateContainer(TContainerHolder &cholder, const rpc::TContainerCreateRequest &req)
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
DestroyContainer(TContainerHolder &cholder, const rpc::TContainerDestroyRequest &req)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> Destroy container: "<<req.name()<<std::endl;

    cholder.Destroy(req.name());
    rsp.set_error(rpc::EContainerError::Success);

    return rsp;
}

static rpc::TContainerResponse
StartContainer(TContainerHolder &cholder, const rpc::TContainerStartRequest &req)
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
StopContainer(TContainerHolder &cholder, const rpc::TContainerStopRequest &req)
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
PauseContainer(TContainerHolder &cholder, const rpc::TContainerPauseRequest &req)
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
ResumeContainer(TContainerHolder &cholder, const rpc::TContainerResumeRequest &req)
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

static rpc::TContainerResponse ListContainers(TContainerHolder &cholder)
{
    rpc::TContainerResponse rsp;

    std::cout<<"-> List containers "<<std::endl;
    rsp.set_error(rpc::EContainerError::Success);

    for (auto name : cholder.List())
        rsp.mutable_list()->add_name(name);

    return rsp;
}

static rpc::TContainerResponse
HandleRpcRequest(TContainerHolder &cholder, const rpc::TContainerRequest &req)
{
    if (req.has_create())
        return CreateContainer(cholder, req.create());

    else if (req.has_destroy())
        return DestroyContainer(cholder, req.destroy());

    else if (req.has_start())
        return StartContainer(cholder, req.start());

    else if (req.has_stop())
        return StopContainer(cholder, req.stop());

    else if (req.has_pause())
        return PauseContainer(cholder, req.pause());

    else if (req.has_resume())
        return ResumeContainer(cholder, req.resume());

    // TODO ...

    else if (req.has_list())
        return ListContainers(cholder);

    // TODO ...

    else {
        rpc::TContainerResponse rsp;
        rsp.set_error(rpc::EContainerError::InvalidMethod);
        return rsp;
    }
}

string HandleRpcRequest(TContainerHolder &cholder, const string msg)
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

int HandleRpcFromSocket(TContainerHolder &cholder, const char *path)
{
    int sfd, cfd;
    struct sockaddr_un my_addr, peer_addr;
    socklen_t peer_addr_size;
    int ret;

    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        std::cerr<<"socket() error: "<<strerror(errno)<<std::endl;
        return -1;
    }

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, path, sizeof(my_addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) < 0) {
        std::cerr<<"bind() error: "<<strerror(errno)<<std::endl;
        return -2;
    }

    if (listen(sfd, 0) < 0) {
        std::cerr<<"listen() error: "<<strerror(errno)<<std::endl;
        return -3;
    }

    peer_addr_size = sizeof(struct sockaddr_un);
    cfd = accept(sfd, (struct sockaddr *) &peer_addr,
                 &peer_addr_size);
    if (cfd < 0) {
        std::cerr<<"accept() error: "<<strerror(errno)<<std::endl;
        return -4;
    }

    __gnu_cxx::stdio_filebuf<char> ibuf(cfd, std::ios_base::in, 1);
    __gnu_cxx::stdio_filebuf<char> obuf(cfd, std::ios_base::out, 1);

    std::istream ist(&ibuf);
    std::ostream ost(&obuf);

    ret = HandleRpcFromStream(cholder, ist, ost);

    ibuf.close();
    obuf.close();

    return ret;
}

int main(int argc, char *argv[])
{
    TContainerHolder cholder;

    //return HandleRpcFromStream(cholder, std::cin, std::cout);
    return HandleRpcFromSocket(cholder, RPC_SOCK_PATH);
}
