#include <vector>
#include <string>
#include <algorithm>
#include <csignal>

#include "portod.hpp"
#include "version.hpp"
#include "statistics.hpp"
#include "rpc.hpp"
#include "holder.hpp"
#include "cgroup.hpp"
#include "config.hpp"
#include "event.hpp"
#include "qdisc.hpp"
#include "context.hpp"
#include "client.hpp"
#include "epoll.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/folder.hpp"
#include "util/protobuf.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/string.hpp"
#include "util/crash.hpp"
#include "util/cred.hpp"
#include "util/worker.hpp"

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
static bool stdlog = false;
static bool failsafe = false;
static bool noNetwork = false;

static void AllocStatistics() {
    Statistics = (TStatistics *)mmap(nullptr, sizeof(*Statistics),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (!Statistics)
        throw std::bad_alloc();
}

static void DaemonOpenLog(bool master) {
    const auto &log = master ? config().master_log() : config().slave_log();

    TLogger::CloseLog();
    TLogger::OpenLog(stdlog, log.path(), log.perm());

    if (!master) {
        TFolder journals(config().journal_dir().path());
        if (!journals.Exists())
            journals.Create(config().journal_dir().perm());
    }
}

static int DaemonSyncConfig(bool master) {
    config.Load();

    if (noNetwork)
        config().mutable_network()->set_enabled(false);
    TNl::EnableDebug(config().network().debug());

    const auto &pid = master ? config().master_pid() : config().slave_pid();

    DaemonOpenLog(master);

    if (CreatePidFile(pid.path(), pid.perm())) {
        L_ERR() << "Can't create pid file " << pid.path() << "!" << std::endl;
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

    L_SYS() << string(80, '-') << std::endl;
    L_SYS() << "Started " << GIT_TAG << " " << GIT_REVISION << " " << GetPid() << (config().container().scoped_unlock() ? " with scoped unlock" : "")<< std::endl;
    L_SYS() << config().DebugString() << std::endl;

    return EXIT_SUCCESS;
}

static void DaemonShutdown(bool master, int ret) {
    const auto &pid = master ? config().master_pid() : config().slave_pid();

    L_SYS() << "Stopped " << ret << std::endl;

    TLogger::CloseLog();
    RemovePidFile(pid.path());

    if (ret < 0)
        RaiseSignal(-ret);
}

static void RemoveRpcServer(const string &path) {
    TFile f(path);
    TError error = f.Remove();
    if (error)
        L_ERR() << "Can't remove socket file: " << error << std::endl;
}

struct TRequest {
    TContext *Context;
    std::shared_ptr<TClient> Client;
    rpc::TContainerRequest Request;
};

class TRpcWorker : public TWorker<TRequest> {
public:
    TRpcWorker(const size_t nr) : TWorker("portod-worker", nr) {}

    const TRequest &Top() override {
        return Queue.front();
    }

    bool Handle(const TRequest &request) override {
        HandleRpcRequest(*request.Context, request.Request, request.Client);

        return true;
    }
};

static bool QueueRequest(TContext &context,
                         std::shared_ptr<TEpollSource> source,
                         TRpcWorker &worker, std::shared_ptr<TClient> client) {
    TRequest req{&context, client};
    bool hangup = false;
    bool fullMessage = client->ReadRequest(req.Request, hangup);

    if (hangup)
        return true;

    if (!fullMessage)
        return false;

    if (client->Identify(*context.Cholder, false))
        return true;

    TError error = context.EpollLoop->DisableSource(source);
    if (error) {
        L_WRN() << "Can't disable client " << client->GetFd() << ": " << error << std::endl;
        return true;
    }

    worker.Push(req);

    return false;
}

static int AcceptClient(TContext &context, int sfd,
                        std::map<int, std::shared_ptr<TClient>> &clients, int &fd) {
    int cfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    peer_addr_size = sizeof(struct sockaddr_un);
    cfd = accept4(sfd, (struct sockaddr *) &peer_addr,
                  &peer_addr_size, SOCK_CLOEXEC);
    if (cfd < 0) {
        if (errno == EAGAIN)
            return 0;

        L_ERR() << "accept() error: " << strerror(errno) << std::endl;
        return -1;
    }

    if (!config().daemon().blocking_write()) {
        struct timeval to;
        to.tv_sec = 30;
        to.tv_usec = 0;

        if (setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, (void *)&to, sizeof(to)) < 0) {
            L_ERR() << "setsockopt() error: " << strerror(errno) << std::endl;
            return -1;
        }
    }

    auto client = std::make_shared<TClient>(context.EpollLoop, cfd);
    TError error = client->Identify(*context.Cholder);
    if (error)
        return -1;

    fd = cfd;
    clients[cfd] = client;
    return 0;
}

static bool AnotherInstanceRunning(const string &path) {
    int fd;
    if (ConnectToRpcServer(path, fd))
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
        L_ERR() << "Can't acknowledge exit status for " << pid << ": " << error << std::endl;
        Crash();
    }
}

static int ReapSpawner(int fd, TContext &context) {
    struct pollfd fds[1];
    int nr = 1000;

    fds[0].fd = fd;
    fds[0].events = POLLIN | POLLHUP;

    while (nr--) {
        int ret = poll(fds, 1, 0);
        if (ret < 0) {
            L_ERR() << "poll() error: " << strerror(errno) << std::endl;
            return ret;
        }

        if (!fds[0].revents || (fds[0].revents & POLLHUP))
            return 0;

        int pid, status;
        if (read(fd, &pid, sizeof(pid)) < 0) {
            L_ERR() << "read(pid): " << strerror(errno) << std::endl;
            return 0;
        }
retry:
        if (read(fd, &status, sizeof(status)) < 0) {
            if (errno == EAGAIN)
                goto retry;
            L_ERR() << "read(status): " << strerror(errno) << std::endl;
            return 0;
        }

        TEvent e(EEventType::Exit);
        e.Exit.Pid = pid;
        e.Exit.Status = status;
        context.Queue->Add(0, e);
    }

    return 0;
}

static inline int EncodeSignal(int sig) {
    return -sig;
}

static void StartWorkers(TContext &context, TRpcWorker &worker) {
    worker.Start();
    context.Queue->Start();
}

static void StopWorkers(TContext &context, TRpcWorker &worker) {
    context.Queue->Stop();
    worker.Stop();
}

static int SlaveRpc(TContext &context, TRpcWorker &worker) {
    int ret = 0;
    int sfd;
    std::map<int, std::shared_ptr<TClient>> clients;
    bool accept_paused = false;

    TCred cred(getuid(), getgid());

    TGroup g(config().rpc_sock().group().c_str());
    TError error = g.Load();
    if (error)
        L_ERR() << "Can't get gid for " << config().rpc_sock().group() << ": " << error << std::endl;

    if (!error)
        cred.Gid = g.GetId();

    error = CreateRpcServer(config().rpc_sock().file().path(),
                            config().rpc_sock().file().perm(),
                            cred.Uid, cred.Gid, sfd);
    if (error) {
        L_ERR() << "Can't create RPC server: " << error.GetMsg() << std::endl;
        return EXIT_FAILURE;
    }

    auto AcceptSource = std::make_shared<TEpollSource>(context.EpollLoop, sfd);
    error = context.EpollLoop->AddSource(AcceptSource);
    if (error) {
        L_ERR() << "Can't add RPC server fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    auto MasterSource = std::make_shared<TEpollSource>(context.EpollLoop, REAP_EVT_FD);
    error = context.EpollLoop->AddSource(MasterSource);
    if (error && !failsafe) {
        L_ERR() << "Can't add master fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    std::shared_ptr<TEpollSource> NetworkSource;
    if (context.NetEvt) {
        NetworkSource = std::make_shared<TEpollSource>(context.EpollLoop,
                                                       context.NetEvt->GetFd());
        error = context.EpollLoop->AddSource(NetworkSource);
        if (error) {
            L_ERR() << "Can't add netlink events fd to epoll: " << error << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::vector<int> signals;
    std::vector<struct epoll_event> events;

    StartWorkers(context, worker);

    bool discardState = false;
    while (true) {
        if (accept_paused && clients.size() * 4 / 3 < config().daemon().max_clients()) {
            L_WRN() << "Resume accepting connections" << std::endl;
            error = context.EpollLoop->AddSource(AcceptSource);
            if (error) {
                L_ERR() << "Can't add RPC server fd to epoll: " << error << std::endl;
                ret = EXIT_FAILURE;
                goto exit;
            }
            accept_paused = false;
        }

        error = context.EpollLoop->GetEvents(signals, events, -1);
        if (error) {
            L_ERR() << "slave: epoll error " << error << std::endl;
            ret = EXIT_FAILURE;
            goto exit;
        }

        for (auto s : signals) {
            switch (s) {
            case SIGINT:
                discardState = true;
                // no break here
            case SIGTERM:
                ret = EncodeSignal(s);
                goto exit;
            case updateSignal:
                L_EVT() << "Updating" << std::endl;
                ret = EncodeSignal(s);
                goto exit;
            case rotateSignal:
                DaemonOpenLog(false);
                break;
            case debugSignal:
                DumpMallocInfo();
                break;
            default:
                /* Ignore other signals */
                break;
            }
        }

        if (!failsafe) {
            ret = ReapSpawner(REAP_EVT_FD, context);
            if (ret)
                goto exit;
        }

        for (auto ev : events) {
            auto source = context.EpollLoop->GetSource(ev.data.ptr);
            if (!source)
                continue;

            if (source->Fd == sfd) {
                if (!accept_paused && clients.size() >= config().daemon().max_clients()) {
                    L_WRN() << "Pause accepting connections" << std::endl;
                    context.EpollLoop->RemoveSource(AcceptSource);
                    accept_paused = true;
                    continue;
                }

                int fd = -1;
                ret = AcceptClient(context, sfd, clients, fd);
                if (ret)
                    goto exit;

                error = context.EpollLoop->AddSource(clients[fd]);
                if (error) {
                    L_ERR() << "Can't add client fd to epoll: " << error << std::endl;
                    ret = EXIT_FAILURE;
                    goto exit;
                }

            } else if (source->Fd == REAP_EVT_FD) {
                // we handled all events from the master before events
                // from the clients (so clients see updated view of the
                // world as soon as possible)
                continue;
            } else if (context.NetEvt && source->Fd == context.NetEvt->GetFd()) {
                L() << "Refresh list of available network interfaces" << std::endl;

                context.NetEvt->FlushEvents();

                TError error = context.Net->Update();
                if (error)
                    L_ERR() << "Can't refresh list of network interfaces: " << error << std::endl;

                TEvent e(EEventType::UpdateNetwork);
                context.Queue->Add(0, e);
            } else if (source->Flags & EPOLL_EVENT_OOM) {
                auto container = source->Container.lock();
                if (container) {
                    TEvent e(EEventType::OOM, container);
                    e.OOM.Fd = source->Fd;
                    context.Queue->Add(0, e);
                }

                // we don't want any repeated events from OOM fd, so remove
                // it from epoll
                context.EpollLoop->RemoveSource(source);

            } else if (clients.find(source->Fd) != clients.end()) {
                auto client = clients[source->Fd];
                bool needClose = false;

                if (ev.events & EPOLLIN)
                    needClose = QueueRequest(context, source, worker, client);

                if ((ev.events & EPOLLHUP) || needClose) {
                    context.EpollLoop->RemoveSource(source);
                    clients.erase(source->Fd);
                }
            } else {
                L_WRN() << "Unknown event " << source->Fd << std::endl;
                context.EpollLoop->RemoveSource(source);
            }
        }
    }

exit:
    StopWorkers(context, worker);

    for (auto pair : clients)
        close(pair.first);

    close(sfd);

    if (discardState)
        context.Destroy();

    return ret;
}

static void KvDump() {
    TLogger::OpenLog(true, "", 0);

    auto containers = std::make_shared<TKeyValueStorage>(TMount("tmpfs", config().keyval().file().path(), "tmpfs", { config().keyval().size() }));
    TError error = containers->MountTmpfs();
    if (error)
        L_ERR() << "Can't mount containers key-value storage: " << error << std::endl;
    else
        containers->Dump();

    auto volumes = std::make_shared<TKeyValueStorage>(TMount("tmpfs", config().volumes().keyval().file().path(), "tmpfs", { config().volumes().keyval().size() }));
    error = volumes->MountTmpfs();
    if (error)
        L_ERR() << "Can't mount volumes key-value storage: " << error << std::endl;
    else
        volumes->Dump();
}

static int TuneLimits() {
    struct rlimit rlim;

    // we need two FDs for each container (to monitor OOM event and
    // to write journal), plus some spare ones
    int maxFd = config().container().max_total() * 2 + 100;

    rlim.rlim_max = maxFd;
    rlim.rlim_cur = maxFd;

    int ret = setrlimit(RLIMIT_NOFILE, &rlim);
    if (ret)
        return EXIT_FAILURE;

    if (config().daemon().debug()) {
        rlim.rlim_max = RLIM_INFINITY;
        rlim.rlim_cur = RLIM_INFINITY;

        ret = setrlimit(RLIMIT_CORE, &rlim);
        if (ret)
            return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static void preParentFork(void) {
    TLogger::DisableLocaltime();
}

static void postParentFork(void) {
    TLogger::EnableLocaltime();
}

static void postChildFork(void) {
    TLogger::ClearBuffer();
}

static int SlaveMain() {
    SetDieOnParentExit(SIGKILL);

    int ret = pthread_atfork(preParentFork, postParentFork, postChildFork);
    if (ret) {
        std::cerr << "Can't set fork handlers: " << strerror(ret) << std::endl;
        return EXIT_FAILURE;
    }

    if (failsafe)
        AllocStatistics();

    Statistics->SlaveStarted = GetCurrentTimeMs();

    ret = DaemonPrepare(false);
    if (ret)
        return ret;

    TRpcWorker worker(config().daemon().workers());

    ret = TuneLimits();
    if (ret) {
        L_ERR() << "Can't set correct limits: " << strerror(errno) << std::endl;
        return ret;
    }

    if (fcntl(REAP_EVT_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR() << "Can't set close-on-exec flag on REAP_EVT_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    if (fcntl(REAP_ACK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR() << "Can't set close-on-exec flag on REAP_ACK_FD: " << strerror(errno) << std::endl;
        if (!failsafe)
            return EXIT_FAILURE;
    }

    umask(0);

    TError error = SetOomScoreAdj(0);
    if (error)
        L_ERR() << "Can't adjust OOM score: " << error << std::endl;

    TContext context;
    try {
        TCgroupSnapshot cs;
        error = cs.Create();
        if (error)
            L_ERR() << "Can't create cgroup snapshot: " << error << std::endl;

        error = context.Initialize();
        if (error) {
            L_ERR() << "Initialization error: " << error << std::endl;
            return EXIT_FAILURE;
        }

        bool restored = context.Cholder->RestoreFromStorage();
        context.Vholder->RestoreFromStorage(context.Cholder);

        L() << "Remove cgroup leftovers..." << std::endl;

        cs.Destroy();

        L() << "Done restoring" << std::endl;

        if (!restored) {
            L() << "Remove container leftovers from previous run..." << std::endl;
            RemoveIf(config().container().tmp_dir(),
                     EFileType::Directory,
                     [](const std::string &name, const TPath &path) {
                        return name != TPath(config().volumes().volume_dir()).BaseName();
                     });
        }

        ret = SlaveRpc(context, worker);
        L_SYS() << "Shutting down..." << std::endl;

        RemoveRpcServer(config().rpc_sock().file().path());
    } catch (string s) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: " << s << std::endl;
        Crash();
    } catch (const char *s) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: " << s << std::endl;
        Crash();
    } catch (const std::exception &exc) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: " << exc.what() << std::endl;
        Crash();
    } catch (...) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: uncaught exception!" << std::endl;
        Crash();
    }

    DaemonShutdown(false, ret);
    context.Destroy();

    return ret;
}

static void DeliverPidStatus(int fd, int pid, int status, size_t queued) {
    L_EVT() << "Deliver " << pid << " status " << status << " (" << queued << " queued)" << std::endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        L_ERR() << "write(pid): " << strerror(errno) << std::endl;
    if (write(fd, &status, sizeof(status)) < 0)
        L_ERR() << "write(status): " << strerror(errno) << std::endl;
}

static void Reap(int pid) {
    (void)waitpid(pid, NULL, 0);
}

static void UpdateQueueSize(map<int,int> &exited) {
    Statistics->QueuedStatuses = exited.size();
}

static int ReapDead(int fd, map<int,int> &exited, int slavePid, int &slaveStatus) {
    while (true) {
        siginfo_t info = { 0 };
        if (waitid(P_ALL, -1, &info, WNOHANG | WNOWAIT | WEXITED) < 0)
            break;

        if (info.si_pid <= 0)
            break;

        int status = 0;
        if (info.si_code == CLD_KILLED) {
            status = info.si_status;
        } else if (info.si_code == CLD_DUMPED) {
            status = info.si_status | (1 << 7);
        } else { // CLD_EXITED
            status = info.si_status << 8;
        }

        if (info.si_pid == slavePid) {
            slaveStatus = status;
            Reap(info.si_pid);
            return -1;
        }

        if (exited.find(info.si_pid) != exited.end())
            break;

        exited[info.si_pid] = status;
        DeliverPidStatus(fd, info.si_pid, status, exited.size());
    }

    UpdateQueueSize(exited);

    return 0;
}

static int ReceiveAcks(int fd, std::map<int,int> &exited) {
    int pid;
    int nr = 0;

    while (read(fd, &pid, sizeof(pid)) == sizeof(pid)) {
        if (pid <= 0)
            continue;

        if (exited.find(pid) == exited.end()) {
            L_WRN() << "Got acknowledge for unknown pid " << pid << std::endl;
        } else {
            exited.erase(pid);
            Reap(pid);
            L_EVT() << "Got acknowledge for " << pid << " (" << exited.size()
                    << " queued" << std::endl;
        }

        nr++;
    }

    UpdateQueueSize(exited);
    return nr;
}

static int SpawnSlave(std::shared_ptr<TEpollLoop> loop, map<int,int> &exited) {
    int evtfd[2];
    int ackfd[2];
    int ret = EXIT_FAILURE;
    TError error;

    slavePid = 0;

    if (pipe2(evtfd, O_NONBLOCK) < 0) {
        L_ERR() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    if (pipe2(ackfd, O_NONBLOCK) < 0) {
        L_ERR() << "pipe(): " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    slavePid = fork();
    if (slavePid < 0) {
        L_ERR() << "fork(): " << strerror(errno) << std::endl;
        ret = EXIT_FAILURE;
        goto exit;
    } else if (slavePid == 0) {
        close(evtfd[1]);
        close(ackfd[0]);
        TLogger::CloseLog();
        loop->Destroy();
        dup2(evtfd[0], REAP_EVT_FD);
        dup2(ackfd[1], REAP_ACK_FD);
        close(evtfd[0]);
        close(ackfd[1]);

        exit(SlaveMain());
    }

    close(evtfd[0]);
    close(ackfd[1]);

    L_SYS() << "Spawned slave " << slavePid << std::endl;
    Statistics->Spawned++;

    for (auto &pair : exited)
        DeliverPidStatus(evtfd[1], pair.first, pair.second, exited.size());

    UpdateQueueSize(exited);

    {
    auto AckSource = std::make_shared<TEpollSource>(loop, ackfd[0]);
    error = loop->AddSource(AckSource);
    if (error) {
        L_ERR() << "Can't add ackfd[0] to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    while (true) {
        std::vector<int> signals;
        std::vector<struct epoll_event> events;

        error = loop->GetEvents(signals, events, -1);
        if (error) {
            L_ERR() << "master: epoll error " << error << std::endl;
            return EXIT_FAILURE;
        }

        for (auto s : signals) {
            switch (s) {
            case SIGINT:
            case SIGTERM:
                if (kill(slavePid, s) < 0)
                    L_ERR() << "Can't send " << s << " to slave" << std::endl;

                L() << "Waiting for slave to exit..." << std::endl;
                (void)RetryFailed(60 * 10, 100,
                [&]() { return waitpid(slavePid, nullptr, WNOHANG) != slavePid; });

                ret = EncodeSignal(s);
                goto exit;
            case debugSignal:
                DumpMallocInfo();

                L() << "Statuses:" << std::endl;
                for (auto pair : exited)
                    L() << pair.first << "=" << pair.second << std::endl;;

                break;
            case updateSignal:
            {
                int ret = DaemonSyncConfig(true);
                if (ret)
                    return ret;

                L_SYS() << "Updating" << std::endl;

                const char *stdlogArg = nullptr;
                if (stdlog)
                    stdlogArg = "--stdlog";

                if (kill(slavePid, updateSignal) < 0) {
                    L_ERR() << "Can't send " << updateSignal << " to slave: " << strerror(errno) << std::endl;
                } else {
                    if (waitpid(slavePid, NULL, 0) != slavePid)
                        L_ERR() << "Can't wait for slave exit status: " << strerror(errno) << std::endl;
                }
                TLogger::CloseLog();
                close(evtfd[1]);
                close(ackfd[0]);
                loop->Destroy();
                execlp(program_invocation_name, program_invocation_name, stdlogArg, nullptr);
                std::cerr << "Can't execlp(" << program_invocation_name << ", " << program_invocation_name << ", NULL)" << strerror(errno) << std::endl;
                ret = EXIT_FAILURE;
                goto exit;
                break;
            }
            case rotateSignal:
                DaemonOpenLog(true);
                break;
            default:
                /* Ignore other signals */
                break;
            }
        }

        for (auto ev : events) {
            auto source = loop->GetSource(ev.data.ptr);
            if (!source)
                continue;

            if (source->Fd == ackfd[0]) {
                if (!ReceiveAcks(ackfd[0], exited)) {
                    ret = EXIT_FAILURE;
                    goto exit;
                }
            } else {
                L() << "Unknown event " << source->Fd << std::endl;
                loop->RemoveSource(source);
            }
        }

        int status;
        if (ReapDead(evtfd[1], exited, slavePid, status)) {
            L_SYS() << "Slave exited with " << status << std::endl;
            ret = EXIT_SUCCESS;
            goto exit;
        }
    }
    }

exit:
    close(evtfd[0]);
    close(evtfd[1]);

    close(ackfd[0]);
    close(ackfd[1]);

    return ret;
}

void CheckVersion(int &prevMaj, int &prevMin) {
    std::string prevVer;

    prevMaj = 0;
    prevMin = 0;

    TFile f(config().version().path(), config().version().perm());

    TError error = f.AsString(prevVer);
    if (!error)
        (void)sscanf(prevVer.c_str(), "v%d.%d", &prevMaj, &prevMin);

    error = f.WriteStringNoAppend(GIT_TAG);
    if (error)
        L_ERR() << "Can't update current version" << std::endl;
}

static int MasterMain(bool respawn) {
    Statistics->MasterStarted = GetCurrentTimeMs();

    int ret = DaemonPrepare(true);
    if (ret)
        return ret;

    int prevMaj, prevMin;
    CheckVersion(prevMaj, prevMin);
    L_SYS() << "Updating from previous version v" << prevMaj << "." << prevMin << std::endl;

    std::shared_ptr<TEpollLoop> ELoop = std::make_shared<TEpollLoop>();
    TError error = ELoop->Create();
    if (error)
        return error;

    // We want propogate mounts into containers
    error = TMount::Remount("/", MS_REC | MS_SHARED);
    if (error) {
        L_ERR() << "Can't remount / recursively as shared" << error << std::endl;
        return EXIT_FAILURE;
    }

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TError error(EError::Unknown, errno, "prctl(PR_SET_CHILD_SUBREAPER)");
        L_ERR() << "Can't set myself as a subreaper, make sure kernel version is at least 3.4: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = SetOomScoreAdj(-1000);
    if (error)
        L_ERR() << "Can't adjust OOM score: " << error << std::endl;

    map<int,int> exited;

    while (true) {
        size_t started = GetCurrentTimeMs();
        size_t next = started + config().container().respawn_delay_ms();
        ret = SpawnSlave(ELoop, exited);
        L() << "Returned " << ret << std::endl;
        if (next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);

        if (slavePid) {
            if (config().daemon().debug()) {
                (void)waitpid(slavePid, nullptr, 0);
            } else {
                (void)kill(slavePid, SIGKILL);
                Reap(slavePid);
            }
        }
        if (ret < 0)
            break;
        if (!respawn)
            break;
    }

    DaemonShutdown(true, ret);

    return ret;
}

bool RunningInContainer() {
    if (getpid() == 1) {
        return getenv("container") != nullptr;
    } else {
        std::string line;
        bool inContainer = false;

        FILE *f = fopen("/proc/1/environ", "r");
        if (!f)
            return false;

        bool done = false;
        while (!done) {
            int c = getc(f);

            if (c == EOF) {
                done = true;
                break;
            } else if (c == '\0') {
                if (StringStartsWith(line, "container=")) {
                    done = true;
                    inContainer = true;
                    break;
                }

                line.clear();
            } else {
                line += (char)c;
            }
        }

        fclose(f);

        return inContainer;
    }
}

int main(int argc, char * const argv[]) {
    bool slaveMode = false;
    bool respawn = true;
    int argn;

    if (getuid() != 0) {
        std::cerr << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    if (RunningInContainer()) {
        std::cerr << "Can't start in container" << std::endl;
        return EXIT_FAILURE;
    }

    AllocStatistics();

    config.Load();

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
        } else if (arg == "--norespawn") {
            respawn = false;
        } else if (arg == "--failsafe") {
            failsafe = true;
        } else if (arg == "--nonet") {
            noNetwork = true;
        } else if (arg == "-t") {
            if (argn + 1 >= argc)
                return EXIT_FAILURE;
            return config.Test(argv[argn + 1]);
        } else {
            std::cerr << "Unknown option " << arg << std::endl;
            return EXIT_FAILURE;
        }
    }

    if (!slaveMode && AnotherInstanceRunning(config().rpc_sock().file().path())) {
        std::cerr << "Another instance of portod is running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (!stdlog) {
        close(0);
        close(1);
        close(2);
        PORTO_ASSERT(open("/dev/null", O_RDONLY) == 0);
        PORTO_ASSERT(open("/dev/null", O_WRONLY) == 1);
        PORTO_ASSERT(open("/dev/null", O_WRONLY) == 2);
    }

    try {
        if (slaveMode)
            return SlaveMain();
        else
            return MasterMain(respawn);
    } catch (std::string s) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: " << s << std::endl;
        Crash();
    } catch (const char *s) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: " << s << std::endl;
        Crash();
    } catch (const std::exception &exc) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: " << exc.what() << std::endl;
        Crash();
    } catch (...) {
        if (config().daemon().debug())
            throw;
        L_ERR() << "EXCEPTION: uncaught exception!" << std::endl;
        Crash();
    }
}

