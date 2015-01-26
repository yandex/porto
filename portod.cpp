#include <vector>
#include <string>
#include <algorithm>
#include <csignal>

#include "portod.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "config.hpp"
#include "event.hpp"
#include "qdisc.hpp"
#include "context.hpp"
#include "client.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/protobuf.hpp"
#include "util/unix.hpp"
#include "util/string.hpp"
#include "util/crash.hpp"
#include "util/cred.hpp"
#include "batch.hpp"

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
#include <sys/mman.h>
#include <grp.h>
#define GNU_SOURCE
#include <sys/socket.h>
#include <sys/resource.h>
}

using std::string;
using std::map;
using std::vector;

static pid_t slavePid;
static volatile sig_atomic_t done = false;
static volatile sig_atomic_t cleanup = true;
static volatile sig_atomic_t rotate = false;
static volatile sig_atomic_t update = false;
static volatile sig_atomic_t raiseSignum = 0;
static bool stdlog = false;
static bool failsafe = false;
static bool noNetwork = false;

const int updateSignal = SIGHUP;
const int rotateSignal = SIGUSR1;
size_t SlaveStarted = 0;
size_t MasterStarted = 0;

std::map<pid_t, posthook_t> posthooks;
static void HandleBatchTasks(int signum, siginfo_t *si, void *unused) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status))
            posthooks[pid](WEXITSTATUS(status));
    }
}

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

static void DoUpdate(int signum) {
    update = true;
}

static void DoRotate(int signum) {
    rotate = true;
}

static void DoNothing(int signum) {
}

static void RaiseSignal(int signum) {
    struct sigaction sa = {};
    sa.sa_handler = SIG_DFL;

    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(rotateSignal, &sa, NULL);
    (void)sigaction(updateSignal, &sa, NULL);
    raise(signum);
    exit(-signum);
}

static void RegisterSignalHandlers() {
    ResetAllSignalHandlers();

    // we may die while writing into communication pipe
    (void)RegisterSignal(SIGPIPE, SIG_IGN);
    // kill all running containers in case of SIGINT (useful for debugging)
    (void)RegisterSignal(SIGINT, DoExitAndCleanup);
    (void)RegisterSignal(updateSignal, DoUpdate);
    (void)RegisterSignal(SIGALRM, DoNothing);
    // don't stop containers when terminating
    (void)RegisterSignal(SIGTERM, DoExit);
    // don't catch SIGQUIT, may be useful to create core dump
    (void)RegisterSignal(rotateSignal, DoRotate);
    (void)RegisterSignal(SIGCHLD, HandleBatchTasks);
}

static void SignalMask(int how) {
    sigset_t mask;
    int sigs[] = { SIGALRM };

    if (sigemptyset(&mask) < 0) {
        L() << "Can't initialize signal mask: " << strerror(errno) << std::endl;
        return;
    }

    for (auto sig: sigs)
        if (sigaddset(&mask, sig) < 0) {
            L() << "Can't add signal to mask: " << strerror(errno) << std::endl;
            return;
        }

    if (sigprocmask(how, &mask, NULL) < 0)
        L() << "Can't set signal mask: " << strerror(errno) << std::endl;
}

TStatistics *Statistics;
static void AllocStatistics() {
    Statistics = (TStatistics *)mmap(nullptr, sizeof(*Statistics),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!Statistics)
        throw std::bad_alloc();
}

static void DaemonRotateLog() {
    if (!stdlog)
        TLogger::CloseLog();
}

static int DaemonSyncConfig(bool master) {
    const auto &oldPid = master ? config().master_pid() : config().slave_pid();
    RemovePidFile(oldPid.path());

    config.Load();
    if (noNetwork)
        config().mutable_network()->set_enabled(false);
    TNl::EnableDebug(config().network().debug());

    const auto &log = master ? config().master_log() : config().slave_log();
    const auto &pid = master ? config().master_pid() : config().slave_pid();

    TLogger::InitLog(log.path(), log.perm());
    if (stdlog)
        TLogger::LogToStd();

    if (CreatePidFile(pid.path(), log.perm())) {
        L() << "Can't create pid file " <<
            pid.path() << "!" << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int DaemonPrepare(bool master) {
    const string procName = master ? "portod" : "portod-slave";

    SetProcessName(procName.c_str());

    int ret = DaemonSyncConfig(master);
    if (ret)
        return ret;

    L() << string(80, '-') << std::endl;
    L() << "Started " << GIT_TAG << " " << GIT_REVISION << std::endl;
    L() << config().DebugString() << std::endl;

    RegisterSignalHandlers();

    if (master)
        (void)RegisterSignal(SIGCHLD, DoNothing);

    return EXIT_SUCCESS;
}

static void DaemonShutdown(bool master) {
    const auto &pid = master ? config().master_pid() : config().slave_pid();

    L() << "Stopped" << std::endl;

    TLogger::CloseLog();
    RemovePidFile(pid.path());
}

static void RemoveRpcServer(const string &path) {
    TFile f(path);
    TError error = f.Remove();
    if (error)
        L_ERR() << "Can't remove socket file: " << error << std::endl;
}

static bool HandleRequest(TContext &context, std::shared_ptr<TClient> client) {
    uint32_t slaveReadTimeout = config().daemon().slave_read_timeout_s();
    InterruptibleInputStream pist(client->Fd);

    rpc::TContainerRequest request;
    rpc::TContainerResponse response;

    if (slaveReadTimeout)
        (void)alarm(slaveReadTimeout);

    SignalMask(SIG_UNBLOCK);
    bool haveData = ReadDelimitedFrom(&pist, &request);
    SignalMask(SIG_BLOCK);

    if (slaveReadTimeout)
        (void)alarm(0);

    if (pist.Interrupted()) {
        L() << "Interrupted read from " << client->Fd << std:: endl;
        return true;
    }

    if (!haveData)
        return true;

    if (HandleRpcRequest(context, request, response, client))
        SendReply(client, response);

    return false;
}

static int IdentifyClient(int fd, std::shared_ptr<TClient> client, int total) {
    struct ucred cr;
    socklen_t len = sizeof(cr);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0) {
        TFile f("/proc/" + std::to_string(cr.pid) + "/comm");
        string comm;

        if (f.AsString(comm))
            comm = "unknown process";

        comm.erase(remove(comm.begin(), comm.end(), '\n'), comm.end());
        L() << comm
            << " (pid " << cr.pid
            << " uid " << cr.uid
            << " gid " << cr.gid
            << ") connected (total " << total + 1 << ")" << std::endl;

        client->Pid = cr.pid;
        client->Cred.Uid = cr.uid;
        client->Cred.Gid = cr.gid;

        return 0;
    } else {
        L() << "unknown process connected" << std::endl;
        return EXIT_FAILURE;
    }
}

static int AcceptClient(int sfd, std::map<int, std::shared_ptr<TClient>> &clients, int &fd) {
    int cfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    peer_addr_size = sizeof(struct sockaddr_un);
    cfd = accept4(sfd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_CLOEXEC);
    if (cfd < 0) {
        if (errno == EAGAIN)
            return 0;

        L() << "accept() error: " << strerror(errno) << std::endl;
        return -1;
    }

    auto client = std::make_shared<TClient>(cfd);
    int ret = IdentifyClient(cfd, client, clients.size());
    if (ret)
        return ret;

    fd = cfd;
    clients[cfd] = client;
    return 0;
}

static void RemoveClient(int cfd, std::map<int, std::shared_ptr<TClient>> &clients) {
    clients.erase(cfd);
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
        L() << "Acknowledge exit status for " << std::to_string(pid) << std::endl;
    } else {
        TError error(EError::Unknown, errno, "write(): returned " + std::to_string(ret));
        if (error)
            L_ERR() << "Can't acknowledge exit status for " << pid << ": " << error << std::endl;
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
            L() << "poll() error: " << strerror(errno) << std::endl;
            return ret;
        }

        if (!fds[0].revents)
            return 0;

        int pid, status;
        if (read(fd, &pid, sizeof(pid)) < 0) {
            L() << "read(pid): " << strerror(errno) << std::endl;
            return 0;
        }
        if (read(fd, &status, sizeof(status)) < 0) {
            L() << "read(status): " << strerror(errno) << std::endl;
            return 0;
        }

        TEvent e(EEventType::Exit);
        e.Exit.Pid = pid;
        e.Exit.Status = status;
        if (!cholder.DeliverEvent(e)) {
            AckExitStatus(pid);
            return 0;
        }
    }

    return 0;
}

static int SlaveRpc(TContext &context) {
    int ret = 0;
    int sfd;
    std::map<int, std::shared_ptr<TClient>> clients;
    TContainerHolder &cholder = *context.Cholder;

    SignalMask(SIG_BLOCK);

    TCred cred(getuid(), getgid());

    TGroup g(config().rpc_sock().group().c_str());
    TError error = g.Load();
    if (error)
        L_ERR() << "Can't get gid for " << config().rpc_sock().group() << ": " << error << std::endl;

    if (!error)
        cred.Gid = g.GetId();

    error = CreateRpcServer(config().rpc_sock().file().path(),
                            config().rpc_sock().file().perm(),
                            cred, sfd);
    if (error) {
        L() << "Can't create RPC server: " << error.GetMsg() << std::endl;
        return EXIT_FAILURE;
    }

    error = EpollAdd(context.Epfd, sfd);
    if (error) {
        L_ERR() << "Can't add RPC server fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = EpollAdd(context.Epfd, REAP_EVT_FD);
    if (error && !failsafe) {
        L_ERR() << "Can't add master fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    if (context.NetEvt) {
        error = EpollAdd(context.Epfd, context.NetEvt->GetFd());
        if (error) {
            L_ERR() << "Can't add netlink events fd to epoll: " << error << std::endl;
            return EXIT_FAILURE;
        }
    }

#define MAX_EVENTS 32
    struct epoll_event ev[MAX_EVENTS];

    while (!done) {
        int timeout = context.Queue->GetNextTimeout();
        Statistics->SlaveTimeoutMs = timeout;
        int nr = epoll_wait(context.Epfd, ev, MAX_EVENTS, timeout);
        if (nr < 0) {
            if (errno != EINTR)
                L() << "epoll() error: " << strerror(errno) << std::endl;

            if (done)
                break;
        }

        if (rotate) {
            rotate = false;
            DaemonRotateLog();
            continue;
        }

        if (update) {
            update = false;

            L() << "Updating" << std::endl;
            TLogger::CloseLog();

            done = true;
            cleanup = false;
            raiseSignum = updateSignal;
            continue;
        }

        context.Queue->DeliverEvents(*context.Cholder);

        for (int i = 0; i < nr; i++) {
            if (ev[i].data.fd == sfd) {
                if (clients.size() > config().daemon().max_clients()) {
                    L() << "Skip connection attempt" << std::endl;
                    continue;
                }

                int fd = -1;
                ret = AcceptClient(sfd, clients, fd);
                if (ret < 0)
                    break;

                error = EpollAdd(context.Epfd, fd);
                if (error) {
                    L_ERR() << "Can't add client fd to epoll: " << error << std::endl;
                    return EXIT_FAILURE;
                }
            } else if (ev[i].data.fd == REAP_EVT_FD) {
                if (failsafe)
                    continue;

                ret = ReapSpawner(REAP_EVT_FD, *context.Cholder);
                if (done)
                    break;
            } else if (context.NetEvt && context.NetEvt->GetFd() == ev[i].data.fd) {
                L() << "Refresh list of available network interfaces" << std::endl;
                context.NetEvt->FlushEvents();

                TError error = context.Net->Update();
                if (error) {
                    L() << "Can't refresh list of network interfaces: " << error << std::endl;
                    break;
                }
            } else if (clients.find(ev[i].data.fd) != clients.end()) {
                auto client = clients[ev[i].data.fd];
                bool needClose = false;

                if (ev[i].events & EPOLLIN)
                    needClose = HandleRequest(context, client);

                if ((ev[i].events & EPOLLHUP) || needClose) {
                    RemoveClient(ev[i].data.fd, clients);
                }
            } else {
                TEvent e(EEventType::OOM);
                e.OOM.Fd = ev[i].data.fd;
                (void)cholder.DeliverEvent(e);
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
    TKeyValueStorage storage(TMount("tmpfs", config().keyval().file().path(), "tmpfs", { config().keyval().size() }));
    TError error = storage.MountTmpfs();
    if (error)
        L_ERR() << "Can't mount key-value storage: " << error << std::endl;
    else
        storage.Dump();
}

static int TuneLimits() {
    struct rlimit rlim;

    // we need FD for each container to monitor OOM event, plus some spare ones
    int maxFd = config().container().max_total() + 100;

    rlim.rlim_max = maxFd;
    rlim.rlim_cur = maxFd;

    int ret = setrlimit(RLIMIT_NOFILE, &rlim);
    if (ret)
        return ret;

    return 0;
}

static int SlaveMain() {
    if (failsafe)
        AllocStatistics();

    Statistics->SlaveStarted = GetCurrentTimeMs();
    SlaveStarted = GetCurrentTimeMs();

    int ret = DaemonPrepare(false);
    if (ret)
        return ret;

    ret = TuneLimits();
    if (ret) {
        L() << "Can't set correct limits: " << strerror(errno) << std::endl;
        return ret;
    }

    if (config().network().enabled()) {
        if (system("modprobe cls_cgroup")) {
            L() << "Can't load cls_cgroup kernel module: " << strerror(errno) << std::endl;
            if (!failsafe)
                return EXIT_FAILURE;

            config().mutable_network()->set_enabled(false);
        }
    }

    if (fcntl(REAP_EVT_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L() << "Can't set close-on-exec flag on REAP_EVT_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    if (fcntl(REAP_ACK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L() << "Can't set close-on-exec flag on REAP_ACK_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    umask(0);

    TError error = SetOomScoreAdj(0);
    if (error)
        L_ERR() << "Can't adjust OOM score: " << error << std::endl;

    try {
        TCgroupSnapshot cs;
        error = cs.Create();
        if (error)
            L_ERR() << "Can't create cgroup snapshot: " << error << std::endl;

        TContext context;
        error = context.Initialize();
        if (error)
            return EXIT_FAILURE;

        bool restored = false;
        {
            std::map<std::string, kv::TNode> m;
            error = context.Storage->Restore(m);
            if (error)
                L_ERR() << "Can't restore state: " << error << std::endl;

            for (auto &r : m) {
                restored = true;
                error = context.Cholder->Restore(r.first, r.second);
                if (error) {
                    L_ERR() << "Can't restore " << r.first << " state : " << error << std::endl;
                    Statistics->RestoreFailed++;
                }
            }
        }

        context.Vholder->RestoreFromStorage();

        L() << "Done restoring" << std::endl;

        cs.Destroy();

        if (!restored) {
            // Remove any container leftovers from previous run
            string path = config().container().tmp_dir();
            TFolder dir(path);
            if (dir.Exists()) {
                L() << "Removing container leftovers from " << path << std::endl;
                TError error = dir.Remove(true);
                if (error)
                    L_ERR() << "Error while removing " << path << " : " << error << std::endl;
            }
        }

        ret = SlaveRpc(context);
        L() << "Shutting down..." << std::endl;

        RemoveRpcServer(config().rpc_sock().file().path());

        if (!cleanup && raiseSignum)
            RaiseSignal(raiseSignum);

        context.Destroy();
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

static void DeliverPidStatus(int fd, int pid, int status, size_t queued) {
    L() << "Deliver " << pid << " status " << status << " (" << queued << " queued)" << std::endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        L() << "write(pid): " << strerror(errno) << std::endl;
    if (write(fd, &status, sizeof(status)) < 0)
        L() << "write(status): " << strerror(errno) << std::endl;
}

static int ReapDead(int fd, map<int,int> &exited, int slavePid, int &slaveStatus, std::set<int> &acked) {
    int status;
    int pid;

    while (true) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;

        if (pid == slavePid) {
            slaveStatus = status;
            return -1;
        }

        if (acked.find(pid) != acked.end()) {
            acked.erase(pid);
            continue;
        }

        exited[pid] = status;
        DeliverPidStatus(fd, pid, status, exited.size());
        Statistics->QueuedStatuses = exited.size();
    }

    return 0;
}

static void ReceiveAcks(int fd, std::map<int,int> &exited,
                        std::set<int> &acked) {
    int pid;

    while (read(fd, &pid, sizeof(pid)) == sizeof(pid)) {
        if (pid <= 0)
            continue;

        if (exited.find(pid) == exited.end())
            acked.insert(pid);
        else
            exited.erase(pid);

        Statistics->QueuedStatuses = exited.size();
        L() << "Got acknowledge for " << pid << " (" << exited.size() << " queued)" << std::endl;
    }
}

static void SaveStatuses(map<int, int> &exited) {
    TFile f(config().daemon().pidmap().path());
    if (f.Exists()) {
        TError error = f.Remove();
        if (error) {
            L_ERR() << "Can't save pid map: " << error << std::endl;
            return;
        }
    }

    for (auto &kv : exited) {
        TError error = f.AppendString(std::to_string(kv.first) + " " + std::to_string(kv.second) + "\n");
        if (error)
            L_ERR() << "Can't save pid map: " << error << std::endl;
    }
}

static void RestoreStatuses(map<int, int> &exited) {
    TFile f(config().daemon().pidmap().path());
    if (!f.Exists())
        return;

    vector<string> lines;
    TError error = f.AsLines(lines);
    if (error) {
        L_ERR() << "Can't restore pid map: " << error << std::endl;
        return;
    }

    for (auto &line : lines) {
        vector<string> tokens;
        error = SplitString(line, ' ', tokens);
        if (error) {
            L_ERR() << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        if (tokens.size() != 2) {
            continue;
        }

        int pid, status;

        error = StringToInt(tokens[0], pid);
        if (error) {
            L_ERR() << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        error = StringToInt(tokens[0], status);
        if (error) {
            L_ERR() << "Can't restore pid map: " << error << std::endl;
            continue;
        }

        exited[pid] = status;
    }
}

static int SpawnSlave(map<int,int> &exited) {
    int evtfd[2];
    int ackfd[2];
    int ret = EXIT_FAILURE;
    int flags;

    slavePid = 0;

    if (pipe2(evtfd, O_NONBLOCK) < 0) {
        L() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    if (pipe2(ackfd, O_NONBLOCK) < 0) {
        L() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    slavePid = fork();
    if (slavePid < 0) {
        L() << "fork(): " << strerror(errno) << std::endl;
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
        L() << "Can't clear O_NONBLOCK flag from ackfd: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    L() << "Spawned slave " << slavePid << std::endl;
    Statistics->Spawned++;

    SignalMask(SIG_BLOCK);

    for (auto &pair : exited)
        DeliverPidStatus(evtfd[1], pair.first, pair.second, exited.size());

    while (!done) {
        if (rotate) {
            rotate = false;
            DaemonRotateLog();
            continue;
        }

        if (update) {
            update = false;

            int ret = DaemonSyncConfig(true);
            if (ret)
                return ret;

            L() << "Updating" << std::endl;

            const char *stdlogArg = nullptr;
            if (stdlog)
                stdlogArg = "--stdlog";

            SaveStatuses(exited);

            if (kill(slavePid, updateSignal) < 0) {
                L() << "Can't send " << updateSignal << " to slave: " << strerror(errno) << std::endl;
            } else {
                if (waitpid(slavePid, NULL, 0) != slavePid)
                    L() << "Can't wait for slave exit status: " << strerror(errno) << std::endl;
            }
            TLogger::CloseLog();
            close(evtfd[1]);
            close(ackfd[0]);
            execlp(program_invocation_name, program_invocation_name, stdlogArg, nullptr);
            L() << "Can't execlp(" << program_invocation_name << ", " << program_invocation_name << ", NULL)" << strerror(errno) << std::endl;
            ret = EXIT_FAILURE;
            break;
        }

        std::set<int> acked;
        SignalMask(SIG_UNBLOCK);
        ReceiveAcks(ackfd[0], exited, acked);
        SignalMask(SIG_BLOCK);

        int status;
        if (ReapDead(evtfd[1], exited, slavePid, status, acked)) {
            L() << "slave exited with " << status << std::endl;
            ret = EXIT_SUCCESS;
            break;
        }
    }

    if (done) {
        if (kill(slavePid, SIGINT) < 0)
            L() << "Can't send SIGINT to slave" << std::endl;

        L() << "Waiting for slave to exit..." << std::endl;
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
    AllocStatistics();
    Statistics->MasterStarted = GetCurrentTimeMs();

    MasterStarted = GetCurrentTimeMs();

    int ret = DaemonPrepare(true);
    if (ret)
        return ret;

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TError error(EError::Unknown, errno, "prctl(PR_SET_CHILD_SUBREAPER,)");
        L_ERR() << "Can't set myself as a subreaper: " << error << std::endl;
        return EXIT_FAILURE;
    }

    TMountSnapshot ms;
    TError error = ms.RemountSlave();
    if (error)
        L_ERR() << "Can't remount shared mountpoints: " << error << std::endl;

    error = SetOomScoreAdj(-1000);
    if (error)
        L_ERR() << "Can't adjust OOM score: " << error << std::endl;

    SignalMask(SIG_UNBLOCK);

    map<int,int> exited;
    RestoreStatuses(exited);

    while (!done) {
        size_t started = GetCurrentTimeMs();
        size_t next = started + config().container().respawn_delay_ms();
        ret = SpawnSlave(exited);
        L() << "Returned " << ret << std::endl;
        if (!done && next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);

        if (slavePid)
            (void)kill(slavePid, SIGKILL);
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

    if (AnotherInstanceRunning(config().rpc_sock().file().path())) {
        L() << "Another instance of portod is running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (slaveMode)
        return SlaveMain();
    else
        return MasterMain();
}
