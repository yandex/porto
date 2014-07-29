#include <iostream>
#include <sstream>

#include "rpc.hpp"
#include "protobuf.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

static int ConnectToRpcServer(const char *path)
{
    int sfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        std::cerr<<"socket() error: "<<strerror(errno)<<std::endl;
        return -1;
    }

    peer_addr.sun_family = AF_UNIX;
    strncpy(peer_addr.sun_path, path, sizeof(peer_addr.sun_path) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(sfd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0) {
        std::cerr<<"connect() error: "<<strerror(errno)<<std::endl;
        return -2;
    }

    return sfd;
}

int main(int argc, char *argv[])
{
    string s;
    int fd = ConnectToRpcServer(RPC_SOCK_PATH);

    if (fd < 0) {
        std::cerr<<"Can't connect to RPC server"<<std::endl;
        return fd;
    }

    if (argc <= 1) {
        std::cerr<<"Please specify message!";
        return -1;
    }

    stringstream msg;
    std::vector<std::string> args(argv + 1, argv + argc);
    copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

    rpc::TContainerRequest request;
    if (!google::protobuf::TextFormat::ParseFromString(msg.str(), &request) ||
        !request.IsInitialized())
        return -1;

    google::protobuf::io::FileInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    rpc::TContainerResponse response;

    writeDelimitedTo(request, &post);
    post.Flush();

    if (readDelimitedFrom(&pist, &response)) {
        google::protobuf::TextFormat::PrintToString(response, &s);
        cout<<s<<endl;
    }

    close(fd);

    return 0;
}
