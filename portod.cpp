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

static const int MAX_CLIENTS = 16;
static const int MAX_CONNECTIONS = MAX_CLIENTS + 1;
static const int POLL_TIMEOUT_MS = 1000;
static const string PID_FILE = "/tmp/porto.pid";

int CreateRpcServer(const string &path)
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
    strncpy(my_addr.sun_path, path.c_str(), sizeof(my_addr.sun_path) - 1);

    unlink(path.c_str());

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

static void RemoveRpcServer(const string &path)
{
    TFile f(path);
    f.Remove();
}

static void HandleRequest(TContainerHolder &cholder, int fd)
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

static int AcceptClient(int sfd, std::vector<int> &clients)
{
    int cfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    peer_addr_size = sizeof(struct sockaddr_un);
    cfd = accept4(sfd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_CLOEXEC);
    if (cfd < 0) {
        if (errno == EAGAIN)
            return 0;

        std::cerr << "accept() error: " << strerror(errno) << std::endl;
        return -1;
    }

    clients.push_back(cfd);
    return 0;
}

static sig_atomic_t done = false;
static int ExitStatus = 0;

static void Stop(int signum)
{
    done = true;
    ExitStatus = -signum;
}

static bool AnotherInstanceRunning(const string &path)
{
    return false; // TODO!!
}

static TError CreatePidFile(const string &path)
{
    TFile f(path);

    return f.WriteStringNoAppend(to_string(getpid()));
}

static void RemovePidFile(const string &path)
{
    TFile f(path);
    f.Remove();
}

static int RpcMain(TContainerHolder &cholder) {
    int ret = 0;
    int sfd;
    std::vector<int> clients;

    sfd = CreateRpcServer(RPC_SOCK_PATH);
    if (sfd < 0) {
        std::cerr << "Can't create RPC server" << std::endl;
        return -1;
    }

    while (!done) {
        struct pollfd fds[MAX_CONNECTIONS];
        memset(fds, 0, sizeof(fds));

        for (size_t i = 0; i < clients.size(); i++) {
            fds[i].fd = clients[i];
            fds[i].events = POLLIN | POLLHUP;
        }

        fds[MAX_CLIENTS].fd = sfd;
        fds[MAX_CLIENTS].events = POLLIN | POLLHUP;

        ret = poll(fds, MAX_CONNECTIONS, POLL_TIMEOUT_MS);
        if (ret < 0) {
            std::cerr << "poll() error: " << strerror(errno) << std::endl;
            break;
        }

        if (fds[MAX_CLIENTS].revents && clients.size() < MAX_CLIENTS) {
            ret = AcceptClient(sfd, clients);
            if (ret < 0)
                break;
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

    for (size_t i = 0; i < clients.size(); i++)
        close(clients[i]);

    close(sfd);

    return ExitStatus;
}

int main(int argc, const char *argv[])
{
    int ret = 0;

    // in case client closes pipe we are writing to in the protobuf code
    signal(SIGPIPE, SIG_IGN);

    // do proper cleanup in case of signal
    signal(SIGQUIT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGINT, Stop);
    signal(SIGHUP, Stop);

    if (AnotherInstanceRunning(PID_FILE)) {
        std::cerr << "Another instance of portod is running!" << std::endl;
        return -1;
    }

    if (CreatePidFile(PID_FILE)) {
        std::cerr << "Can't create pid file " << PID_FILE << "!" << std::endl;
        return -1;
    }

    try {
        TKeyValueStorage storage;
        storage.MountTmpfs();

        TContainerHolder cholder;
        TCgroupSnapshot cs;

        for (auto &r : storage.Restore())
            // TODO: handle error
            cholder.Restore(r.first, r.second);

        ret = RpcMain(cholder);
    } catch (string s) {
        cout << s << endl;
        ret = EXIT_FAILURE;
    } catch (const char *s) {
        cout << s << endl;
        ret = EXIT_FAILURE;
    } catch (...) {
        cout << "Uncaught exception!" << endl;
        ret = EXIT_FAILURE;
    }

    RemovePidFile(PID_FILE);
    RemoveRpcServer(RPC_SOCK_PATH);

    return ret;
}
