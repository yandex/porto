#include "rpc.h"
#include "fdstream.h"

extern "C" {
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
    strncpy(peer_addr.sun_path, RPC_SOCK_PATH, sizeof(peer_addr.sun_path) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(sfd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0) {
        std::cerr<<"connect() error: "<<strerror(errno)<<std::endl;
        return -2;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int fd = ConnectToRpcServer(RPC_SOCK_PATH);

    if (fd < 0) {
        std::cerr<<"Can't connect to RPC server"<<std::endl;
        return fd;
    }

    FdStream str(fd);

    str.ost<<"list: {}"<<std::endl;
    str.ost.flush();

    for (std::string msg; std::getline(str.ist, msg);) {
            cout<<msg<<std::endl;
    }

    return 0;
}
