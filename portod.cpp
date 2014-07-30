#include "protobuf.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "threadpool.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

static int CreateRpcServer(const char *path)
{
    int fd;
    struct sockaddr_un my_addr;

    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr<<"socket() error: "<<strerror(errno)<<std::endl;
        return -1;
    }

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, path, sizeof(my_addr.sun_path) - 1);

    unlink(path);

    if (bind(fd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) < 0) {
        std::cerr<<"bind() error: "<<strerror(errno)<<std::endl;
        return -2;
    }

    if (listen(fd, 0) < 0) {
        std::cerr<<"listen() error: "<<strerror(errno)<<std::endl;
        return -3;
    }

    return fd;
}

void HandleConnection(TContainerHolder &cholder, int fd)
{
    google::protobuf::io::FileInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    rpc::TContainerRequest request;
    while (readDelimitedFrom(&pist, &request)) {
        auto rsp = HandleRpcRequest(cholder, request);

        if (rsp.IsInitialized()) {
            writeDelimitedTo(rsp, &post);
            post.Flush();
        }
    }

    close(fd);
}

int rpc_main(TContainerHolder &cholder) {
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;
    int sfd;
    int cfd;

    sfd = CreateRpcServer(RPC_SOCK_PATH);
    if (sfd < 0) {
        std::cerr << "Can't create RPC server" << std::endl;
        return -1;
    }

    TThreadPool pool(16);

    while (true) {
        peer_addr_size = sizeof(struct sockaddr_un);
        cfd = accept(sfd, (struct sockaddr *) &peer_addr,
                     &peer_addr_size);
        if (cfd < 0) {
            std::cerr << "accept() error: " << strerror(errno) << std::endl;
            break;
        }

        pool.Enqueue([&cholder, cfd]{ HandleConnection(cholder, cfd); });
    }

    close(sfd);

    return 0;
}

int main(int argc, const char *argv[])
{
    try {
        TContainerHolder cholder;
        TCgroupState cs;

        while (1) {
            cs.MountMissingTmpfs();
            cs.UpdateFromProcFs();
            cs.MountMissingControllers();
            cs.UpdateFromProcFs();
            cs.UmountAll();
            cs.UpdateFromProcFs();
        }

        throw;
        return rpc_main(cholder);
    } catch (...) {
        return EXIT_FAILURE;
    }
}
