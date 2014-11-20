#include <vector>
#include <string>
#include <algorithm>
#include <csignal>

#include "porto.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "config.hpp"
#include "event.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/unix.hpp"
#include "util/string.hpp"
#include "util/crash.hpp"
#include "util/pwd.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/epoll.h>
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

static pid_t slavePid;
static volatile sig_atomic_t done = false;
static volatile sig_atomic_t cleanup = true;
static volatile sig_atomic_t hup = false;
static volatile sig_atomic_t usr1 = false;
static volatile sig_atomic_t raiseSignum = 0;
static bool stdlog = false;
static bool failsafe = false;
static bool noNetwork = false;

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

static void DoSigusr1(int signum) {
    usr1 = true;
}

static void DoNothing(int signum) {
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
    (void)RegisterSignal(SIGALRM, DoNothing);
    // don't stop containers when terminating
    (void)RegisterSignal(SIGTERM, DoExit);
    // don't catch SIGQUIT, may be useful to create core dump
    (void)RegisterSignal(SIGUSR1, DoSigusr1);
}

static void SignalMask(int how) {
    sigset_t mask;
    int sigs[] = { SIGALRM, SIGCHLD };

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

static void DaemonTruncLog() {
    if (!stdlog) {
        TLogger::CloseLog();
        TLogger::TruncateLog();
        TLogger::Log() << "Truncated log" << std::endl;
    }
}

static int DaemonSyncConfig(bool master, bool trunc, bool reset) {
    const auto &oldPid = master ? config().master_pid() : config().slave_pid();
    RemovePidFile(oldPid.path());

    if (trunc)
        DaemonTruncLog();

    if (reset) {
        StatReset(PORTO_STAT_SPAWNED);
        StatReset(PORTO_STAT_ERRORS);
        StatReset(PORTO_STAT_WARNS);
    }

    config.Load();
    if (noNetwork)
        config().mutable_network()->set_enabled(false);
    TNl::EnableDebug(config().network().debug());

    const auto &log = master ? config().master_log() : config().slave_log();
    const auto &pid = master ? config().master_pid() : config().slave_pid();

    TLogger::InitLog(log.path(), log.perm(), config().log().verbose());
    if (stdlog)
        TLogger::LogToStd();

    if (CreatePidFile(pid.path(), log.perm())) {
        TLogger::Log() << "Can't create pid file " <<
            pid.path() << "!" << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int DaemonPrepare(bool master) {
    const string procName = master ? "portod" : "portod-slave";

    SetProcessName(procName.c_str());

    int ret = DaemonSyncConfig(master, false, master);
    if (ret)
        return ret;

    TLogger::Log() << string(80, '-') << std::endl;
    TLogger::Log() << "Started " << GIT_TAG << " " << GIT_REVISION << std::endl;
    TLogger::Log() << config().DebugString() << std::endl;

    RegisterSignalHandlers();

    if (master)
        (void)RegisterSignal(SIGCHLD, DoNothing);

    return EXIT_SUCCESS;
}

static void DaemonShutdown(bool master) {
    const auto &pid = master ? config().master_pid() : config().slave_pid();

    TLogger::Log() << "Stopped" << std::endl;

    TLogger::CloseLog();
    RemovePidFile(pid.path());
}

static void RemoveRpcServer(const string &path) {
    TFile f(path);
    TError error = f.Remove();
    if (error)
        TLogger::Log(LOG_ERROR) << "Can't remove socket file: " << error << std::endl;
}

struct ClientInfo {
    int Pid;
    int Uid;
    int Gid;
};

static bool HandleRequest(TContainerHolder &cholder, const int fd,
                          const int uid, const int gid) {
    InterruptibleInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    rpc::TContainerRequest request;

    (void)alarm(config().daemon().slave_read_timeout_s());
    SignalMask(SIG_UNBLOCK);
    bool haveData = ReadDelimitedFrom(&pist, &request);
    SignalMask(SIG_BLOCK);
    (void)alarm(0);

    if (pist.Interrupted()) {
        TLogger::Log() << "Interrupted read from " << fd << std:: endl;
        return true;
    }

    if (haveData) {
        auto rsp = HandleRpcRequest(cholder, request, uid, gid);

        if (rsp.IsInitialized()) {
            if (!WriteDelimitedTo(rsp, &post))
                TLogger::Log() << "Write error for " << fd << std:: endl;
            post.Flush();
        }
    } else {
        TLogger::Log() << "Read nothing from " << fd << std:: endl;
        return true;
    }

    return false;
}

static int IdentifyClient(int fd, ClientInfo &ci, int total) {
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
            << ") connected (total " << total + 1 << ")" << std::endl;

        ci.Pid = cr.pid;
        ci.Uid = cr.uid;
        ci.Gid = cr.gid;

        return 0;
    } else {
        TLogger::Log() << "unknown process connected" << std::endl;
        return EXIT_FAILURE;
    }
}

static int AcceptClient(int sfd, std::map<int,ClientInfo> &clients, int &fd) {
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

    ClientInfo ci;
    int ret = IdentifyClient(cfd, ci, clients.size());
    if (ret)
        return ret;

    fd = cfd;
    clients[cfd] = ci;
    return 0;
}

static void RemoveClient(int cfd, std::map<int,ClientInfo> &clients) {
    ClientInfo ci = clients.at(cfd);
    clients.erase(cfd);

    TLogger::Log() << "pid " << ci.Pid
        << " uid " << ci.Uid
        << " gid " << ci.Gid
        << " disconnected (total " << clients.size() << ")" << std::endl;
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
        if (error)
            TLogger::Log(LOG_ERROR) << "Can't acknowledge exit status for " << pid << ": " << error << std::endl;
        if (ret < 0)
            Crash();
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

        TEvent event(pid, status);
        if (!cholder.DeliverEvent(event)) {
            AckExitStatus(pid);
            return 0;
        }
    }

    return 0;
}

static int RpcMain(std::shared_ptr<TEventQueue> queue, TContainerHolder &cholder) {
    int ret = 0;
    int sfd;
    std::map<int,ClientInfo> clients;

    SignalMask(SIG_BLOCK);

    uid_t uid = getuid();
    gid_t gid = getgid();

    TGroup g(config().rpc_sock().group().c_str());
    TError error = g.Load();
    if (error)
        TLogger::Log(LOG_ERROR) << "Can't get gid for " << config().rpc_sock().group() << ": " << error << std::endl;

    if (!error)
        gid = g.GetId();

    error = CreateRpcServer(config().rpc_sock().file().path(),
                            config().rpc_sock().file().perm(),
                            uid, gid, sfd);
    if (error) {
        TLogger::Log() << "Can't create RPC server: " << error.GetMsg() << std::endl;
        return EXIT_FAILURE;
    }

    error = EpollAdd(cholder.Epfd, sfd);
    if (error) {
        TLogger::Log(LOG_ERROR) << "Can't add RPC server fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = EpollAdd(cholder.Epfd, REAP_EVT_FD);
    if (error && !failsafe) {
        TLogger::Log(LOG_ERROR) << "Can't add master fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

#define MAX_EVENTS 32
    struct epoll_event ev[MAX_EVENTS];

    while (!done) {
        int nr = epoll_wait(cholder.Epfd, ev, MAX_EVENTS, queue->GetNextTimeout());
        if (nr < 0) {
            TLogger::Log() << "epoll() error: " << strerror(errno) << std::endl;

            if (done)
                break;
        }

        if (hup) {
            close(sfd);

            RemoveRpcServer(config().rpc_sock().file().path());

            int ret = DaemonSyncConfig(false, true, false);
            if (ret)
                return ret;
            hup = false;
            TLogger::Log() << "Syncing config" << std::endl;

            TError error = CreateRpcServer(config().rpc_sock().file().path(),
                                           config().rpc_sock().file().perm(),
                                           uid, gid, sfd);
            if (error) {
                TLogger::Log() << "Can't create RPC server: " << error.GetMsg() << std::endl;
                return EXIT_FAILURE;
            }

            error = EpollAdd(cholder.Epfd, sfd);
            if (error) {
                TLogger::Log(LOG_ERROR) << "Can't add RPC server fd to epoll: " << error << std::endl;
                return EXIT_FAILURE;
            }

            continue;
        }

        if (usr1) {
            DaemonTruncLog();

            TLogger::Log() << "Updating" << std::endl;

            done = true;
            cleanup = false;
            raiseSignum = SIGUSR1;
            continue;
        }

        queue->DeliverEvents(cholder);

        for (int i = 0; i < nr; i++) {
            if (ev[i].data.fd == sfd) {
                if (clients.size() > config().daemon().max_clients()) {
                    TLogger::Log() << "Skip connection attempt" << std::endl;
                    continue;
                }

                int fd = -1;
                ret = AcceptClient(sfd, clients, fd);
                if (ret < 0)
                    break;

                error = EpollAdd(cholder.Epfd, fd);
                if (error) {
                    TLogger::Log(LOG_ERROR) << "Can't add client fd to epoll: " << error << std::endl;
                    return EXIT_FAILURE;
                }
            } else if (ev[i].data.fd == REAP_EVT_FD) {
                if (failsafe)
                    continue;

                ret = ReapSpawner(REAP_EVT_FD, cholder);
                if (done)
                    break;
            } else if (clients.find(ev[i].data.fd) != clients.end()) {
                auto &ci = clients.at(ev[i].data.fd);
                bool needClose = false;

                if (ev[i].events & EPOLLIN)
                    needClose = HandleRequest(cholder, ev[i].data.fd,
                                              ci.Uid, ci.Gid);

                if ((ev[i].events & EPOLLHUP) || needClose) {
                    close(ev[i].data.fd);
                    RemoveClient(ev[i].data.fd, clients);
                }
            } else {
                TEvent e(ev[i].data.fd);
                cholder.DeliverEvent(e);
            }
        }
    }

    for (auto pair : clients)
        close(pair.first);

    close(sfd);

    SignalMask(SIG_UNBLOCK);

    return ret;
}

static void KvDump() {
    TKeyValueStorage storage;
    storage.Dump();
}

static int SlaveMain() {
    int ret = DaemonPrepare(false);
    if (ret)
        return ret;

    if (AnotherInstanceRunning(config().rpc_sock().file().path())) {
        TLogger::Log() << "Another instance of portod is running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (config().network().enabled()) {
        if (system("modprobe cls_cgroup")) {
            TLogger::Log() << "Can't load cls_cgroup kernel module: " << strerror(errno) << std::endl;
            if (!failsafe)
                return EXIT_FAILURE;

            config().mutable_network()->set_enabled(false);
        }
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

    TError error = SetOomScoreAdj(0);
    if (error)
        TLogger::Log(LOG_ERROR) << "Can't adjust OOM score: " << error << std::endl;

    try {
        TKeyValueStorage storage;
        // don't fail, try to recover anyway
        TError error = storage.MountTmpfs();
        if (error)
            TLogger::Log(LOG_ERROR) << "Can't create key-value storage, skipping recovery: " << error << std::endl;

        TCgroupSnapshot cs;
        error = cs.Create();
        if (error)
            TLogger::Log(LOG_ERROR) << "Can't create cgroup snapshot: " << error << std::endl;

        std::vector<std::shared_ptr<TNlLink>> links;
        if (config().network().enabled()) {
            links = OpenLinks();
            if (links.size() == 0) {
                TLogger::Log() << "Error: couldn't find suitable network interface" << std::endl;
                return EXIT_FAILURE;
            }

            for (auto &link : links)
                TLogger::Log() << "Using " << link->GetName() << " interface" << std::endl;
        }

        auto queue = std::make_shared<TEventQueue>();
        TContainerHolder cholder(queue, links);
        error = cholder.CreateRoot();
        if (error) {
            TLogger::Log(LOG_ERROR) << "Can't create root container: " << error << std::endl;
            return EXIT_FAILURE;
        }

        bool restored = false;
        {
            std::map<std::string, kv::TNode> m;
            error = storage.Restore(m);
            if (error)
                TLogger::Log(LOG_ERROR) << "Can't restore state: " << error << std::endl;

            for (auto &r : m) {
                restored = true;
                error = cholder.Restore(r.first, r.second);
                if (error)
                    TLogger::Log(LOG_ERROR) << "Can't restore " << r.first << "state : " << error << std::endl;
            }
        }

        cs.Destroy();

        if (!restored) {
            // Remove any container leftovers from previous run
            string path = config().container().tmp_dir();
            TFolder dir(path);
            if (dir.Exists()) {
                TLogger::Log() << "Removing container leftovers from " << path << std::endl;
                TError error = dir.Remove(true);
                if (error)
                    TLogger::Log(LOG_ERROR) << "Error while removing " << path << " : " << error << std::endl;
            }
        }

        ret = RpcMain(queue, cholder);
        TLogger::Log() << "Shutting down..." << std::endl;

        RemoveRpcServer(config().rpc_sock().file().path());

        if (!cleanup && raiseSignum)
            RaiseSignal(raiseSignum);

        error = storage.Destroy();
        if (error)
            TLogger::Log(LOG_ERROR) << "Can't destroy key-value storage: " << error << std::endl;
    } catch (string s) {
        std::cerr << s << std::endl;
        ret = EXIT_FAILURE;
    } catch (const char *s) {
        std::cerr << s << std::endl;
        ret = EXIT_FAILURE;
    } catch (const std::exception &exc) {
        std::cerr << exc.what() << std::endl;
        ret = EXIT_FAILURE;
    } catch (...) {
        std::cerr << "Uncaught exception!" << std::endl;
        ret = EXIT_FAILURE;
    }

    DaemonShutdown(false);

    if (raiseSignum)
        RaiseSignal(raiseSignum);

    return ret;
}

static void SendPidStatus(int fd, int pid, int status, size_t queued) {
    TLogger::Log() << "Deliver " << pid << " status " << status << " (" << queued << " queued)" << std::endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        TLogger::Log() << "write(pid): " << strerror(errno) << std::endl;
    if (write(fd, &status, sizeof(status)) < 0)
        TLogger::Log() << "write(status): " << strerror(errno) << std::endl;
}

static int SendPids(int fd, map<int,int> &pidToStatus, int slavePid, int &slaveStatus) {
    int status;
    int pid;

    while (true) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            return 0;

        if (pid == slavePid) {
            slaveStatus = status;
            return -1;
        }

        SendPidStatus(fd, pid, status, pidToStatus.size());
        pidToStatus[pid] = status;
    }

    return -1;
}

static void ReceiveAcks(int fd, map<int,int> &pidToStatus) {
    int pid;

    while (read(fd, &pid, sizeof(pid)) == sizeof(pid)) {
        if (errno == EINTR)
            return;

        pidToStatus.erase(pid);
        TLogger::Log() << "Got acknowledge for " << pid << " (" << pidToStatus.size() << " queued)" << std::endl;
    }
}

static void SavePidMap(map<int, int> &pidToStatus) {
    TFile f(config().daemon().pidmap().path());
    if (f.Exists()) {
        TError error = f.Remove();
        if (error) {
            TLogger::Log(LOG_ERROR) << "Can't save pid map: " << error << std::endl;
            return;
        }
    }

    for (auto &kv : pidToStatus) {
        TError error = f.AppendString(std::to_string(kv.first) + " " + std::to_string(kv.second) + "\n");
        TLogger::Log(LOG_ERROR) << "Can't save pid map: " << error << std::endl;
    }
}

static void RestorePidMap(map<int, int> &pidToStatus) {
    TFile f(config().daemon().pidmap().path());
    if (!f.Exists())
        return;

    vector<string> lines;
    TError error = f.AsLines(lines);
    if (error) {
        TLogger::Log(LOG_ERROR) << "Can't restore pid map: " << error << std::endl;
        return;
    }

    for (auto &line : lines) {
        vector<string> tokens;
        error = SplitString(line, ' ', tokens);
        if (error) {
            TLogger::Log(LOG_ERROR) << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        if (tokens.size() != 2) {
            continue;
        }

        int pid, status;

        error = StringToInt(tokens[0], pid);
        if (error) {
            TLogger::Log(LOG_ERROR) << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        error = StringToInt(tokens[0], status);
        if (error) {
            TLogger::Log(LOG_ERROR) << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        pidToStatus[pid] = status;
    }
}

static int SpawnSlave(map<int,int> &pidToStatus) {
    int evtfd[2];
    int ackfd[2];
    int ret = EXIT_FAILURE;
    int flags;

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

        exit(SlaveMain());
    }

    close(evtfd[0]);
    close(ackfd[1]);

    flags = fcntl(ackfd[0], F_GETFL, 0);
    if (flags < 0 || fcntl(ackfd[0], F_SETFL, flags & (~O_NONBLOCK)) < 0) {
        TLogger::Log() << "Can't clear O_NONBLOCK flag from ackfd: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    TLogger::Log() << "Spawned slave " << slavePid << std::endl;
    StatInc(PORTO_STAT_SPAWNED);

    SignalMask(SIG_BLOCK);

    for (auto &pair : pidToStatus)
        SendPidStatus(evtfd[1], pair.first, pair.second, pidToStatus.size());

    while (!done) {
        if (usr1) {
            usr1 = false;
            int ret = DaemonSyncConfig(true, true, true);
            if (ret)
                return ret;

            TLogger::Log() << "Updating" << std::endl;

            const char *stdlogArg = nullptr;
            if (stdlog)
                stdlogArg = "--stdlog";

            SavePidMap(pidToStatus);

            if (kill(slavePid, SIGUSR1) < 0)
                TLogger::Log() << "Can't send SIGUSR1 to slave: " << strerror(errno) << std::endl;
            if (waitpid(slavePid, NULL, 0) != slavePid)
                TLogger::Log() << "Can't wait for slave exit status: " << strerror(errno) << std::endl;
            TLogger::CloseLog();
            close(evtfd[1]);
            close(ackfd[0]);
            execlp(program_invocation_name, program_invocation_name, stdlogArg, nullptr);
            TLogger::Log() << "Can't execlp(" << program_invocation_name << ", " << program_invocation_name << ", NULL)" << strerror(errno) << std::endl;
            ret = EXIT_FAILURE;
            break;
        }

        if (hup) {
            hup = false;
            int ret = DaemonSyncConfig(true, true, false);
            if (ret) {
                TLogger::Log(LOG_ERROR) << "Can't sync config" << std::endl;
                return ret;
            }
        }

        SignalMask(SIG_UNBLOCK);
        ReceiveAcks(ackfd[0], pidToStatus);
        SignalMask(SIG_BLOCK);

        int status;
        if (SendPids(evtfd[1], pidToStatus, slavePid, status)) {
            TLogger::Log() << "slave exited with " << status << std::endl;
            ret = EXIT_SUCCESS;
            break;
        }
    }

    if (done) {
        if (kill(slavePid, SIGINT) < 0)
            TLogger::Log() << "Can't send SIGINT to slave" << std::endl;

        TLogger::Log() << "Waiting for slave to exit..." << std::endl;
        (void)waitpid(slavePid, nullptr, 0);
    }

exit:
    close(evtfd[0]);
    close(evtfd[1]);

    close(ackfd[0]);
    close(ackfd[1]);

    SignalMask(SIG_UNBLOCK);

    return ret;
}

static int MasterMain() {
    int ret = DaemonPrepare(true);
    if (ret)
        return ret;

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TError error(EError::Unknown, errno, "prctl(PR_SET_CHILD_SUBREAPER,)");
        TLogger::Log(LOG_ERROR) << "Can't set myself as a subreaper: " << error << std::endl;
        return EXIT_FAILURE;
    }

    TMountSnapshot ms;
    TError error = ms.RemountSlave();
    if (error)
        TLogger::Log(LOG_ERROR) << "Can't remount shared mountpoints: " << error << std::endl;

    error = SetOomScoreAdj(-1000);
    if (error)
        TLogger::Log(LOG_ERROR) << "Can't adjust OOM score: " << error << std::endl;

    SignalMask(SIG_UNBLOCK);

    map<int,int> pidToStatus;
    RestorePidMap(pidToStatus);

    while (!done) {
        size_t started = GetCurrentTimeMs();
        size_t next = started + config().container().respawn_delay_ms();
        ret = SpawnSlave(pidToStatus);
        TLogger::Log() << "Returned " << ret << std::endl;

        if (!done && next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);
    }

    DaemonShutdown(true);

    return ret;
}

int main(int argc, char * const argv[]) {
    bool slaveMode = false;
    int argn;

    if (getuid() != 0) {
        std::cerr << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    for (argn = 1; argn < argc; argn++) {
        string arg(argv[argn]);

        if (arg == "-v" || arg == "--version") {
            std::cout << GIT_TAG << " " << GIT_REVISION <<std::endl;
            return EXIT_SUCCESS;
        } else if (arg == "--kv-dump") {
            config.Load();
            KvDump();
            return EXIT_SUCCESS;
        } else if (arg == "--slave") {
            slaveMode = true;
        } else if (arg == "--stdlog") {
            stdlog = true;
        } else if (arg == "--failsafe") {
            failsafe = true;
        } else if (arg == "--nonet") {
            noNetwork = true;
        } else if (arg == "-t") {
            if (argn + 1 >= argc)
                return EXIT_FAILURE;
            return config.Test(argv[argn + 1]);
        }
    }

    if (slaveMode)
        return SlaveMain();
    else
        return MasterMain();
}
