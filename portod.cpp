#include <vector>

#include "protobuf.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
}

static int CreateRpcServer(const char *path)
{
    int fd;
    struct sockaddr_un my_addr;

    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        std::cerr << "socket() error: " << strerror(errno) << std::endl;
        return -1;
    }

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, path, sizeof(my_addr.sun_path) - 1);

    unlink(path);

    if (bind(fd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) < 0) {
        std::cerr << "bind() error: " << strerror(errno) << std::endl;
        return -2;
    }

    if (listen(fd, 0) < 0) {
        std::cerr << "listen() error: " << strerror(errno) << std::endl;
        return -3;
    }

    return fd;
}

void HandleRequest(TContainerHolder &cholder, int fd)
{
    google::protobuf::io::FileInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    rpc::TContainerRequest request;
    if (readDelimitedFrom(&pist, &request)) {
        auto rsp = HandleRpcRequest(cholder, request);

        if (rsp.IsInitialized()) {
            writeDelimitedTo(rsp, &post);
            post.Flush();
        }
    }
}

int AcceptClient(int sfd, std::vector<int> &clients)
{
    int cfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    peer_addr_size = sizeof(struct sockaddr_un);
    cfd = accept4(sfd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_CLOEXEC);
    if (cfd < 0) {
        std::cerr << "accept() error: " << strerror(errno) << std::endl;
        return -1;
    }

    clients.push_back(cfd);
    return 0;
}

#define MAX_CLIENTS     (16)
#define MAX_CONNECTIONS (MAX_CLIENTS + 1)

int rpc_main(TContainerHolder &cholder) {
    int sfd;
    std::vector<int> clients;

    sfd = CreateRpcServer(RPC_SOCK_PATH);
    if (sfd < 0) {
        std::cerr << "Can't create RPC server" << std::endl;
        return -1;
    }

    while (true) {
        struct pollfd fds[MAX_CONNECTIONS];
        memset(fds, 0, sizeof(fds));

        for (size_t i = 0; i < clients.size(); i++) {
            fds[i].fd = clients[i];
            fds[i].events = POLLIN | POLLHUP;
        }

        fds[MAX_CLIENTS].fd = sfd;
        fds[MAX_CLIENTS].events = POLLIN | POLLHUP;

        int ret = poll(fds, MAX_CONNECTIONS, 1000);
        if (ret < 0) {
            std::cerr << "poll() error: " << strerror(errno) << std::endl;
            break;
        }

        if (fds[MAX_CLIENTS].revents && clients.size() < MAX_CLIENTS) {
            ret = AcceptClient(sfd, clients);
            if (ret && ret != EAGAIN)
                return ret;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!fds[i].revents)
                continue;

            if (fds[i].revents & POLLIN)
                HandleRequest(cholder, fds[i].fd);

            if (fds[i].revents & POLLHUP) {
                close(fds[i].fd);
                clients.erase(std::remove(clients.begin(), clients.end(),
                                          fds[i].fd));
            }
        }
    }

    close(sfd);

    return 0;
}

int main(int argc, const char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    try {
        TContainerHolder cholder;
        TCgroupSnapshot cs;

        return rpc_main(cholder);
    } catch (string s) {
        cout << s << endl;
        return EXIT_FAILURE;
    } catch (const char *s) {
        cout << s << endl;
        return EXIT_FAILURE;
    } catch (...) {
        cout << "Uncaught exception!" << endl;
        return EXIT_FAILURE;
    }
}
