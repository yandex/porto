#include <vector>
#include <algorithm>

#include "version.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "log.hpp"
#include "util/protobuf.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/types.h>
#include <signal.h>
}

using namespace std;

static const size_t MAX_CLIENTS = 16;
static const size_t MAX_CONNECTIONS = MAX_CLIENTS + 1;
static const size_t POLL_TIMEOUT_MS = 1000;
static const string PID_FILE = "/run/portod.pid";
static const string LOG_FILE = "/var/log/portod";

static void RemoveRpcServer(const string &path)
{
    TFile f(path);
    (void)f.Remove();
}

static void HandleRequest(TContainerHolder &cholder, int fd)
{
    google::protobuf::io::FileInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    rpc::TContainerRequest request;
    if (ReadDelimitedFrom(&pist, &request)) {
        auto rsp = HandleRpcRequest(cholder, request);

        if (rsp.IsInitialized()) {
            WriteDelimitedTo(rsp, &post);
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

static volatile sig_atomic_t Done = false;
static volatile sig_atomic_t Cleanup = true;
static volatile sig_atomic_t Hup = false;
static volatile sig_atomic_t RaiseSignum = 0;

static void DoExit(int signum)
{
    Done = true;
    Cleanup = false;
    RaiseSignum = signum;
    TLogger::CloseLog();
}

static void DoExitAndCleanup(int signum)
{
    Done = true;
    Cleanup = true;
    RaiseSignum = signum;
    TLogger::CloseLog();
}

static void DoHangup(int signum)
{
    Hup = true;
}

static bool AnotherInstanceRunning(const string &path)
{
    int fd;
    TError error = ConnectToRpcServer(path, fd);

    if (error)
        return false;

    close(fd);
    return true;
}

static TError CreatePidFile(const string &path)
{
    TFile f(path);

    return f.WriteStringNoAppend(to_string(getpid()));
}

static void RemovePidFile(const string &path)
{
    TFile f(path);
    (void)f.Remove();
}

static int RpcMain(TContainerHolder &cholder) {
    int ret = 0;
    int sfd;
    std::vector<int> clients;

    TError error = CreateRpcServer(RPC_SOCK_PATH, sfd);
    if (error) {
        cerr << "Can't create RPC server: " << error.GetMsg() << endl;
        return -1;
    }

    while (!Done) {
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

            if (!Hup)
                break;
        }

        if (Hup) {
            TLogger::CloseLog();
            TLogger::OpenLog(LOG_FILE);
            Hup = false;
        }

        if (fds[MAX_CLIENTS].revents && clients.size() < MAX_CLIENTS) {
            ret = AcceptClient(sfd, clients);
            if (ret < 0)
                break;
        }

        for (size_t i = 0; i < MAX_CLIENTS; i++) {
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

    return ret;
}

static void ReaiseSignal(int signum) {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    raise(signum);
}

void KvDump() {
        TKeyValueStorage storage;
        storage.Dump();
}

int main(int argc, char * const argv[])
{
    int ret = 0;
    int opt;

    while ((opt = getopt(argc, argv, "dv")) != -1) {
        switch (opt) {
        case 'd':
            KvDump();
            return EXIT_SUCCESS;
        case 'v':
            cout << GIT_REVISION <<endl;
            return EXIT_FAILURE;
        default:
            exit(EXIT_FAILURE);
        }
    }

    // in case client closes pipe we are writing to in the protobuf code
    signal(SIGPIPE, SIG_IGN);

    // don't stop containers when terminating
    // don't catch SIGQUIT, may be useful to create core dump
    signal(SIGTERM, DoExit);
    signal(SIGHUP, DoHangup);

    // kill all running containers in case of SIGINT (useful for debugging)
    signal(SIGINT, DoExitAndCleanup);

    if (AnotherInstanceRunning(RPC_SOCK_PATH)) {
        std::cerr << "Another instance of portod is running!" << std::endl;
        return -1;
    }

    if (CreatePidFile(PID_FILE)) {
        std::cerr << "Can't create pid file " << PID_FILE << "!" << std::endl;
        return -1;
    }

    TLogger::OpenLog(LOG_FILE);

    try {
        TKeyValueStorage storage;
        // don't fail, try to recover anyway
        (void)storage.MountTmpfs();

        TContainerHolder cholder;
        {
            TCgroupSnapshot cs;
            std::map<std::string, kv::TNode> m;

            TError error = storage.Restore(m);
            if (error) {
                cerr << "Couldn't restore state!" << endl;
                    // TODO: report user?!
            }

            for (auto &r : m) {
                TError e = cholder.Restore(r.first, r.second);
                if (e) {
                    cerr << "Couldn't restore " << r.first << " state!" << endl;
                    // TODO: report user?!
                }
            }
        }

        ret = RpcMain(cholder);
        if (!Cleanup && RaiseSignum)
            ReaiseSignal(RaiseSignum);
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

    if (RaiseSignum)
        ReaiseSignal(RaiseSignum);

    return ret;
}
