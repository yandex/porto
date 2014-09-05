#include <vector>
#include <algorithm>
#include <csignal>

#include "porto.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
}

using namespace std;

static const size_t MAX_CONNECTIONS = PORTOD_MAX_CLIENTS + 1;

static void RemoveRpcServer(const string &path) {
    TFile f(path);
    (void)f.Remove();
}

static void HandleRequest(TContainerHolder &cholder, int fd) {
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

static int AcceptClient(int sfd, std::vector<int> &clients) {
    int cfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    peer_addr_size = sizeof(struct sockaddr_un);
    cfd = accept4(sfd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_CLOEXEC);
    if (cfd < 0) {
        if (errno == EAGAIN)
            return 0;

        TLogger::Log() << "accept() error: " << strerror(errno) << endl;
        return -1;
    }

    clients.push_back(cfd);
    return 0;
}

static volatile sig_atomic_t done = false;
static volatile sig_atomic_t cleanup = true;
static volatile sig_atomic_t hup = false;
static volatile sig_atomic_t raiseSignum = 0;

static void DoExit(int signum) {
    done = true;
    cleanup = false;
    raiseSignum = signum;
}

static void DoExitAndCleanup(int signum) {
    done = true;
    cleanup = true;
    raiseSignum = signum;
}

static void DoHangup(int signum) {
    hup = true;
}

static bool AnotherInstanceRunning(const string &path) {
    int fd;
    TError error = ConnectToRpcServer(path, fd);

    if (error)
        return false;

    close(fd);
    return true;
}

static TError CreatePidFile(const string &path, const int mode) {
    TFile f(path, mode);

    return f.WriteStringNoAppend(to_string(getpid()));
}

static void RemovePidFile(const string &path) {
    TFile f(path);
    (void)f.Remove();
}

void AckExitStatus(int pid) {
    if (!pid)
        return;

    int ret = write(REAP_ACK_FD, &pid, sizeof(pid));
    if (ret == sizeof(pid)) {
        TLogger::Log() << "Acknowledge exit status for " << to_string(pid) << endl;
    } else {
        TError error(EError::Unknown, errno, "write(): returned " + to_string(ret));
        TLogger::LogError(error, "Can't acknowledge exit status for " + to_string(pid));
    }
}

static int ReapSpawner(int fd, TContainerHolder &cholder) {
    struct pollfd fds[1];
    int nr = 1000;

    fds[0].fd = fd;
    fds[0].events = POLLIN | POLLHUP;

    while (nr--) {
        int ret = poll(fds, 1, 0);
        if (ret < 0) {
            TLogger::Log() << "poll() error: " << strerror(errno) << endl;
            return ret;
        }

        if (!fds[0].revents)
            return 0;

        int pid, status;
        if (read(fd, &pid, sizeof(pid)) < 0) {
            TLogger::Log() << "read(pid): " << strerror(errno) << endl;
            return 0;
        }
        if (read(fd, &status, sizeof(status)) < 0) {
            TLogger::Log() << "read(status): " << strerror(errno) << endl;
            return 0;
        }

        if (!cholder.DeliverExitStatus(pid, status)) {
            TLogger::Log() << "Can't deliver " << to_string(pid) << " exit status " << to_string(status) << endl;
            AckExitStatus(pid);
            return 0;
        }
    }

    return 0;
}

static int RpcMain(TContainerHolder &cholder) {
    int ret = 0;
    int sfd;
    std::vector<int> clients;

    uid_t uid = getuid();
    gid_t gid = getgid();
    struct group *g = getgrnam(RPC_SOCK_GROUP.c_str());
    if (g)
        gid = g->gr_gid;
    else
        TLogger::Log() << "Can't get gid for " << RPC_SOCK_GROUP << " group" << endl;

    TError error = CreateRpcServer(RPC_SOCK, RPC_SOCK_PERM, uid, gid, sfd);
    if (error) {
        TLogger::Log() << "Can't create RPC server: " << error.GetMsg() << endl;
    }

    int starved = 0;
    size_t heartbeat = 0;
    while (!done) {
        struct pollfd fds[MAX_CONNECTIONS];
        memset(fds, 0, sizeof(fds));

        for (size_t i = 0; i < clients.size(); i++) {
            fds[i].fd = clients[i];
            fds[i].events = POLLIN | POLLHUP;
        }

        fds[PORTOD_MAX_CLIENTS].fd = sfd;
        fds[PORTOD_MAX_CLIENTS].events = POLLIN | POLLHUP;

        ret = poll(fds, MAX_CONNECTIONS, PORTOD_POLL_TIMEOUT_MS);
        if (ret < 0) {
            TLogger::Log() << "poll() error: " << strerror(errno) << endl;

            if (done)
                break;
        }

        if (heartbeat + HEARTBEAT_DELAY_MS <= GetCurrentTimeMs()) {
            cholder.Heartbeat();
            heartbeat = GetCurrentTimeMs();
        }

        if (hup) {
            TLogger::CloseLog();
            hup = false;
        }

        ret = ReapSpawner(REAP_EVT_FD, cholder);
        if (done)
            break;

        if (fds[PORTOD_MAX_CLIENTS].revents && clients.size() < PORTOD_MAX_CLIENTS) {
            ret = AcceptClient(sfd, clients);
            if (ret < 0)
                break;
        }

        for (size_t i = 0; i < PORTOD_MAX_CLIENTS && !done; i++) {
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
    struct sigaction sa = {};
    sa.sa_handler = SIG_DFL;

    TLogger::CloseLog();

    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);
    raise(signum);
    exit(-signum);
}

static void KvDump() {
        TKeyValueStorage storage;
        storage.Dump();
}

int main(int argc, char * const argv[])
{
    int ret = EXIT_SUCCESS;
    int opt;

    while ((opt = getopt(argc, argv, "dv")) != -1) {
        switch (opt) {
        case 'd':
            KvDump();
            return EXIT_SUCCESS;
        case 'v':
            cout << GIT_TAG << " " << GIT_REVISION <<endl;
            return EXIT_FAILURE;
        default:
            exit(EXIT_FAILURE);
        }
    }

    TLogger::InitLog(LOG_FILE, LOG_FILE_PERM);

    umask(0);

    if (fcntl(REAP_EVT_FD, F_SETFD, FD_CLOEXEC) < 0) {
        TLogger::Log() << "Can't set close-on-exec flag on REAP_EVT_FD: " << strerror(errno) << endl;
        return EXIT_FAILURE;
    }

    if (fcntl(REAP_ACK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        TLogger::Log() << "Can't set close-on-exec flag on REAP_ACK_FD: " << strerror(errno) << endl;
        return EXIT_FAILURE;
    }

    // in case client closes pipe we are writing to in the protobuf code
    (void)RegisterSignal(SIGPIPE, SIG_IGN);

    // don't stop containers when terminating
    // don't catch SIGQUIT, may be useful to create core dump
    (void)RegisterSignal(SIGTERM, DoExit);
    (void)RegisterSignal(SIGHUP, DoHangup);

    // kill all running containers in case of SIGINT (useful for debugging)
    (void)RegisterSignal(SIGINT, DoExitAndCleanup);

    if (AnotherInstanceRunning(RPC_SOCK)) {
        TLogger::Log() << "Another instance of portod is running!" << endl;
        return EXIT_FAILURE;
    }

    if (CreatePidFile(PID_FILE, PID_FILE_PERM)) {
        TLogger::Log() << "Can't create pid file " << PID_FILE << "!" << endl;
        return EXIT_FAILURE;
    }

    TLogger::Log() << "Started" << endl;
    try {
        TKeyValueStorage storage;
        // don't fail, try to recover anyway
        TError error = storage.MountTmpfs();
        TLogger::LogError(error, "Couldn't create key-value storage, skipping recovery");

        TContainerHolder cholder;
        error = cholder.CreateRoot();
        if (error)
            TLogger::LogError(error, "Couldn't create root container!");

        {
            TCgroupSnapshot cs;
            TError error = cs.Create();
            if (error)
                TLogger::LogError(error, "Couldn't create cgroup snapshot!");

            std::map<std::string, kv::TNode> m;
            error = storage.Restore(m);
            if (error)
                TLogger::LogError(error, "Couldn't restore state!");

            for (auto &r : m) {
                error = cholder.Restore(r.first, r.second);
                if (error)
                    TLogger::LogError(error, string("Couldn't restore ") + r.first + " state!");
            }
        }

        ret = RpcMain(cholder);
        if (!cleanup && raiseSignum)
            ReaiseSignal(raiseSignum);
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
    RemoveRpcServer(RPC_SOCK);

    if (raiseSignum)
        ReaiseSignal(raiseSignum);

    return ret;
}
