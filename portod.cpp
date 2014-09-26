#include <vector>
#include <string>
#include <algorithm>
#include <csignal>

#include "porto.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/unix.hpp"
#include "util/crash.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <grp.h>
#define GNU_SOURCE
#include <sys/socket.h>
}

using std::string;
using std::map;
using std::vector;

static const size_t MAX_CONNECTIONS = PORTOD_MAX_CLIENTS + 1;

static pid_t slavePid;
static volatile sig_atomic_t done = false;
static volatile sig_atomic_t gotAlarm = false;
static volatile sig_atomic_t cleanup = true;
static volatile sig_atomic_t hup = false;
static volatile sig_atomic_t raiseSignum = 0;
static bool stdlog = false;

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

static void DoAlarm(int signum) {
    // we just need to interrupt poll
    gotAlarm = true;
}

static void RaiseSignal(int signum) {
    struct sigaction sa = {};
    sa.sa_handler = SIG_DFL;

    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGHUP, &sa, NULL);
    raise(signum);
    exit(-signum);
}

static void RegisterSignalHandlers() {
    ResetAllSignalHandlers();

    // we may die while writing into communication pipe
    (void)RegisterSignal(SIGPIPE, SIG_IGN);
    // kill all running containers in case of SIGINT (useful for debugging)
    (void)RegisterSignal(SIGINT, DoExitAndCleanup);
    (void)RegisterSignal(SIGHUP, DoHangup);
    (void)RegisterSignal(SIGALRM, DoAlarm);
    // don't stop containers when terminating
    (void)RegisterSignal(SIGTERM, DoExit);
    // don't catch SIGQUIT, may be useful to create core dump
}

static int DaemonPrepare(const std::string &procName,
                   const std::string &logPath,
                   unsigned int logMode,
                   const std::string &pidPath,
                   unsigned int pidMode) {
    SetProcessName(procName.c_str());

    TLogger::InitLog(logPath, logMode);
    if (stdlog)
        TLogger::LogToStd();

    TLogger::Log() << "--------------------------------------------------------------------------------" << std::endl;
    TLogger::Log() << "Started" << std::endl;

    RegisterSignalHandlers();

    if (CreatePidFile(pidPath, pidMode)) {
        TLogger::Log() << "Can't create pid file " << pidPath << "!" << std::endl;
        return EXIT_FAILURE;
    }

    return 0;
}

static void DaemonShutdown(const std::string &pidPath) {
    TLogger::Log() << "Stopped" << std::endl;

    TLogger::CloseLog();
    RemovePidFile(pidPath);
}

static void RemoveRpcServer(const string &path) {
    TFile f(path);
    (void)f.Remove();
}

static void HandleRequest(TContainerHolder &cholder, int fd) {
    NonblockingInputStream pist(fd);
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

static void IdentifyClient(int fd) {
    struct ucred cr;
    socklen_t len = sizeof(cr);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0) {
        TFile client("/proc/" + std::to_string(cr.pid) + "/comm");
        string comm;

        if (client.AsString(comm))
            comm = "unknown process";

        comm.erase(remove(comm.begin(), comm.end(), '\n'), comm.end());
        TLogger::Log() << comm
            << " (pid " << cr.pid
            << " uid " << cr.uid
            << " gid " << cr.gid
            << ") connected" << std::endl;
    } else
        TLogger::Log() << "unknown process connected" << std::endl;
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

        TLogger::Log() << "accept() error: " << strerror(errno) << std::endl;
        return -1;
    }

    IdentifyClient(cfd);
    clients.push_back(cfd);

    return 0;
}

static bool AnotherInstanceRunning(const string &path) {
    int fd;
    TError error = ConnectToRpcServer(path, fd);

    if (error)
        return false;

    close(fd);
    return true;
}

void AckExitStatus(int pid) {
    if (!pid)
        return;

    int ret = write(REAP_ACK_FD, &pid, sizeof(pid));
    if (ret == sizeof(pid)) {
        TLogger::Log() << "Acknowledge exit status for " << std::to_string(pid) << std::endl;
    } else {
        TError error(EError::Unknown, errno, "write(): returned " + std::to_string(ret));
        TLogger::LogError(error, "Can't acknowledge exit status for " + std::to_string(pid));
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
            TLogger::Log() << "poll() error: " << strerror(errno) << std::endl;
            return ret;
        }

        if (!fds[0].revents)
            return 0;

        int pid, status;
        if (read(fd, &pid, sizeof(pid)) < 0) {
            TLogger::Log() << "read(pid): " << strerror(errno) << std::endl;
            return 0;
        }
        if (read(fd, &status, sizeof(status)) < 0) {
            TLogger::Log() << "read(status): " << strerror(errno) << std::endl;
            return 0;
        }

        if (!cholder.DeliverExitStatus(pid, status)) {
            TLogger::Log() << "Can't deliver " << std::to_string(pid) << " exit status " << std::to_string(status) << std::endl;
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
        TLogger::Log() << "Can't get gid for " << RPC_SOCK_GROUP << " group" << std::endl;

    TError error = CreateRpcServer(RPC_SOCK, RPC_SOCK_PERM, uid, gid, sfd);
    if (error) {
        TLogger::Log() << "Can't create RPC server: " << error.GetMsg() << std::endl;
    }

    size_t heartbeat = 0;
    vector<struct pollfd> fds;

    while (!done) {
        struct pollfd pfd = {};

        fds.clear();

        pfd.fd = sfd;
        pfd.events = POLLIN | POLLHUP;
        fds.push_back(pfd);

        for (size_t i = 0; i < clients.size(); i++) {
            pfd.fd = clients[i];
            pfd.events = POLLIN | POLLHUP;
            fds.push_back(pfd);
        }

        ret = poll(fds.data(), fds.size(), PORTOD_POLL_TIMEOUT_MS);
        if (ret < 0) {
            TLogger::Log() << "poll() error: " << strerror(errno) << std::endl;

            if (done)
                break;
        }

        if (heartbeat + HEARTBEAT_DELAY_MS <= GetCurrentTimeMs()) {
            cholder.Heartbeat();
            heartbeat = GetCurrentTimeMs();
            WatchdogStrobe();
        }

        if (hup) {
            TLogger::TruncateLog();
            hup = false;
        }

        ret = ReapSpawner(REAP_EVT_FD, cholder);
        if (done)
            break;

        if (fds[0].revents && clients.size() < PORTOD_MAX_CLIENTS) {
            ret = AcceptClient(sfd, clients);
            if (ret < 0)
                break;
        }

        for (size_t i = 1; i < fds.size() && !done; i++) {
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

static void KvDump() {
    TKeyValueStorage storage;
    storage.Dump();
}

static int SlaveMain(bool failsafe, bool noWatchdog) {
    int ret = DaemonPrepare("portod-slave",
                            LOG_FILE, LOG_FILE_PERM,
                            PID_FILE, PID_FILE_PERM);
    if (ret)
        return ret;

    if (AnotherInstanceRunning(RPC_SOCK)) {
        TLogger::Log() << "Another instance of portod is running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (system("modprobe cls_cgroup")) {
        TLogger::Log() << "Can't load cls_cgroup kernel module: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    if (fcntl(REAP_EVT_FD, F_SETFD, FD_CLOEXEC) < 0) {
        TLogger::Log() << "Can't set close-on-exec flag on REAP_EVT_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    if (fcntl(REAP_ACK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        TLogger::Log() << "Can't set close-on-exec flag on REAP_ACK_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    umask(0);

    try {
        TKeyValueStorage storage;
        // don't fail, try to recover anyway
        TError error = storage.MountTmpfs();
        TLogger::LogError(error, "Couldn't create key-value storage, skipping recovery");

        TCgroupSnapshot cs;
        error = cs.Create();
        if (error)
            TLogger::LogError(error, "Couldn't create cgroup snapshot!");

        TContainerHolder cholder;
        error = cholder.CreateRoot();
        if (error) {
            TLogger::LogError(error, "Couldn't create root container!");
            return EXIT_FAILURE;
        }

        {
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

        cs.Destroy();

        if (!noWatchdog)
            WatchdogStart();

        ret = RpcMain(cholder);
        if (!cleanup && raiseSignum)
            RaiseSignal(raiseSignum);

        error = storage.Destroy();
        TLogger::LogError(error, "Couldn't destroy key-value storage");
    } catch (string s) {
        std::cout << s << std::endl;
        ret = EXIT_FAILURE;
    } catch (const char *s) {
        std::cout << s << std::endl;
        ret = EXIT_FAILURE;
    } catch (...) {
        std::cout << "Uncaught exception!" << std::endl;
        ret = EXIT_FAILURE;
    }

    RemoveRpcServer(RPC_SOCK);

    if (raiseSignum)
        RaiseSignal(raiseSignum);

    DaemonShutdown(PID_FILE);

    return ret;
}

static void SendPidStatus(int fd, int pid, int status, size_t queued) {
    TLogger::Log() << "Deliver " << pid << " status " << status << " (" + std::to_string(queued) + " queued)" << std::endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        TLogger::Log() << "write(pid): " << strerror(errno) << std::endl;
    if (write(fd, &status, sizeof(status)) < 0)
        TLogger::Log() << "write(status): " << strerror(errno) << std::endl;
}

static void ReceiveAcks(int fd, map<int,int> &pidToStatus) {
    int pid;

    while (read(fd, &pid, sizeof(pid)) == sizeof(pid)) {
        TLogger::Log() << "Got acknowledge for " << pid << std::endl;
        pidToStatus.erase(pid);
    }
}

static void SignalMask(int how) {
    sigset_t mask;
    int sigs[] = { SIGALRM };

    if (sigemptyset(&mask) < 0) {
        TLogger::Log() << "Can't initialize signal mask: " << strerror(errno) << std::endl;
        return;
    }

    for (auto sig: sigs)
        if (sigaddset(&mask, sig) < 0) {
            TLogger::Log() << "Can't add signal to mask: " << strerror(errno) << std::endl;
            return;
        }

    if (sigprocmask(how, &mask, NULL) < 0)
        TLogger::Log() << "Can't set signal mask: " << strerror(errno) << std::endl;
}

static int SpawnPortod(map<int,int> &pidToStatus) {
    int evtfd[2];
    int ackfd[2];
    int ret = EXIT_FAILURE;

    if (pipe(evtfd) < 0) {
        TLogger::Log() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    if (pipe2(ackfd, O_NONBLOCK) < 0) {
        TLogger::Log() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    slavePid = fork();
    if (slavePid < 0) {
        TLogger::Log() << "fork(): " << strerror(errno) << std::endl;
        ret = EXIT_FAILURE;
        goto exit;
    } else if (slavePid == 0) {
        close(evtfd[1]);
        close(ackfd[0]);
        TLogger::CloseLog();
        dup2(evtfd[0], REAP_EVT_FD);
        dup2(ackfd[1], REAP_ACK_FD);
        close(evtfd[0]);
        close(ackfd[1]);

        exit(SlaveMain(false, false));
    }

    close(evtfd[0]);
    close(ackfd[1]);

    TLogger::Log() << "Spawned slave " << slavePid << std::endl;

    for (auto &pair : pidToStatus)
        SendPidStatus(evtfd[1], pair.first, pair.second, pidToStatus.size());

    SignalMask(SIG_BLOCK);

    while (!done) {
        int pid;

        ReceiveAcks(ackfd[0], pidToStatus);

        if (hup) {
            TLogger::TruncateLog();
            TLogger::Log() << "Updating" << std::endl;

            if (kill(slavePid, SIGKILL) < 0)
                TLogger::Log() << "Can't send SIGKILL to slave: " << strerror(errno) << std::endl;
            if (waitpid(slavePid, NULL, 0) != slavePid)
                TLogger::Log() << "Can't wait for slave exit status: " << strerror(errno) << std::endl;
            TLogger::CloseLog();
            close(evtfd[1]);
            close(ackfd[0]);
            execlp(program_invocation_name, program_invocation_name, nullptr);
            TLogger::Log() << "Can't execlp(" << program_invocation_name << ", " << program_invocation_name << ", NULL)" << strerror(errno) << std::endl;
            ret = EXIT_FAILURE;
            break;
        }

        SignalMask(SIG_UNBLOCK);
        (void)alarm(LOOP_WAIT_TIMEOUT_S);
        int status;
        pid = wait(&status);
        (void)alarm(0);
        SignalMask(SIG_BLOCK);

        if (gotAlarm) {
            gotAlarm = false;
            continue;
        }

        if (errno == EINTR) {
            TLogger::Log() << "wait(): " << strerror(errno) << std::endl;
            continue;
        }
        if (pid == slavePid) {
            TLogger::Log() << "slave exited with " << status << std::endl;
            ret = EXIT_SUCCESS;
            break;
        }

        SendPidStatus(evtfd[1], pid, status, pidToStatus.size());
        pidToStatus[pid] = status;
    }

    SignalMask(SIG_UNBLOCK);

    ReceiveAcks(ackfd[0], pidToStatus);

exit:
    close(evtfd[0]);
    close(evtfd[1]);

    close(ackfd[0]);
    close(ackfd[1]);

    return ret;
}

static int MasterMain() {
    int ret = DaemonPrepare("portod",
                            LOOP_LOG_FILE, LOOP_LOG_FILE_PERM,
                            LOOP_PID_FILE, LOOP_PID_FILE_PERM);
    if (ret)
        return ret;

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TLogger::Log() << "Can't set myself as a subreaper" << std::endl;
        return EXIT_FAILURE;
    }

    TMountSnapshot ms;
    TError error = ms.RemountSlave();
    if (error)
        TLogger::LogError(error, "Can't remount shared mountpoints");

    SignalMask(SIG_UNBLOCK);

    map<int,int> pidToStatus;
    while (!done) {
        size_t started = GetCurrentTimeMs();
        size_t next = started + RESPAWN_DELAY_MS;
        ret = SpawnPortod(pidToStatus);
        TLogger::Log() << "Returned " << ret << std::endl;

        if (!done && next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);
    }

    if (kill(slavePid, SIGINT) < 0)
        TLogger::Log() << "Can't send SIGINT to slave" << std::endl;

    DaemonShutdown(LOOP_PID_FILE);

    return ret;
}

int main(int argc, char * const argv[]) {
    bool slaveMode = false;
    bool failsafe = false;
    bool noWatchdog = false;
    int argn;

    if (getuid() != 0) {
        TLogger::Log() << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    for (argn = 1; argn < argc; argn++) {
        string arg(argv[argn]);

        if (arg == "-v" || arg == "--version") {
            std::cout << GIT_TAG << " " << GIT_REVISION <<std::endl;
            return EXIT_SUCCESS;
        } else if (arg == "--kv-dump") {
            KvDump();
            return EXIT_SUCCESS;
        } else if (arg == "--slave") {
            slaveMode = true;
        } else if (arg == "--stdlog") {
            stdlog = true;
        } else if (arg == "--failsafe") {
            failsafe = true;
        } else if (arg == "--nowatch") {
            noWatchdog = true;
        }
    }

    if (slaveMode)
        return SlaveMain(failsafe, noWatchdog);
    else
        return MasterMain();
}
