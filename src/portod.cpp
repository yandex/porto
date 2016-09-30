#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <iostream>

#include "version.hpp"
#include "statistics.hpp"
#include "kvalue.hpp"
#include "rpc.hpp"
#include "cgroup.hpp"
#include "config.hpp"
#include "event.hpp"
#include "network.hpp"
#include "client.hpp"
#include "epoll.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/worker.hpp"
#include "property.hpp"
#include "portod.hpp"

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

std::string PreviousVersion;

std::unique_ptr<TEpollLoop> EpollLoop;
std::unique_ptr<TEventQueue> EventQueue;

static pid_t slavePid;
static bool stdlog = false;
static bool failsafe = false;

static void FatalError(const std::string &text, TError &error) {
    L_ERR() << text << ": " << error << std::endl;
    _exit(EXIT_FAILURE);
}

static void AllocStatistics() {
    Statistics = (TStatistics *)mmap(nullptr, sizeof(*Statistics),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    PORTO_ASSERT(Statistics != nullptr);
}

static void DaemonOpenLog(bool master) {
    const auto &log = master ? config().master_log() : config().slave_log();

    TLogger::CloseLog();
    TLogger::OpenLog(stdlog, log.path(), log.perm());
}

static int DaemonSyncConfig(bool master) {
    config.Load();

    const auto &pid = master ? config().master_pid() : config().slave_pid();

    DaemonOpenLog(master);

    TPath pidPath(pid.path());
    if (!pidPath.Exists())
        pidPath.Mkfile(pid.perm());

    if (pidPath.WriteAll(std::to_string(getpid()))) {
        L_ERR() << "Can't create pid file " << pid.path() << "!" << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int DaemonPrepare(bool master) {
    const std::string procName = master ? "portod" : "portod-slave";

    SetProcessName(procName.c_str());

    int ret = DaemonSyncConfig(master);
    if (ret)
        return ret;

    L_SYS() << std::string(80, '-') << std::endl;
    L_SYS() << "Started " << PORTO_VERSION << " " << PORTO_REVISION << " " << GetPid() << std::endl;
    L_SYS() << config().DebugString() << std::endl;

    return EXIT_SUCCESS;
}

static void DaemonShutdown(bool master, int ret) {
    TPath pidFile(master ? config().master_pid().path() : config().slave_pid().path());

    L_SYS() << "Stopped " << ret << std::endl;

    TLogger::CloseLog();
    (void)pidFile.Unlink();

    if (ret < 0) {
        int sig = -ret;
        Signal(sig, SIG_DFL);
        raise(sig);
        if (master)
            exit(128 + sig);
        else
            _exit(128 + sig);
    }
}

struct TRequest {
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
        HandleRpcRequest(request.Request, request.Client);
        Statistics->RequestsCompleted++;
        Statistics->RequestsQueued--;

        return true;
    }
};

static TError CreatePortoSocket() {
    TPath path(PORTO_SOCKET_PATH);
    struct sockaddr_un addr;
    TError error;
    TFile sock;

    if (dup2(PORTO_SK_FD, PORTO_SK_FD) == PORTO_SK_FD) {
        struct stat fd_stat, sk_stat;

        if (fstat(PORTO_SK_FD, &fd_stat) || !S_ISSOCK(fd_stat.st_mode))
            Crash();

        if (!path.StatStrict(sk_stat) && S_ISSOCK(sk_stat.st_mode)) {
            time_t now = time(nullptr);
            L_SYS() << "Reuse porto socket: "
                    << "inode " << fd_stat.st_ino << ":" << sk_stat.st_ino
                    << " age " << now - fd_stat.st_ctime << ":" << now - sk_stat.st_ctime
                    << std::endl;
            return TError::Success();
        }

        L_WRN() << "Unlinked porto socket. Recreating..." << std::endl;
    }

    sock.SetFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock.Fd < 0)
        return TError(EError::Unknown, errno, "socket()");

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    (void)path.Unlink();

    if (fchmod(sock.Fd, PORTO_SOCKET_MODE) < 0)
        return TError(EError::Unknown, errno, "fchmod()");

    if (bind(sock.Fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        return TError(EError::Unknown, errno, "bind()");

    error = path.Chown(RootUser, PortoGroup);
    if (error)
        return error;

    error = path.Chmod(PORTO_SOCKET_MODE);
    if (error)
        return error;

    if (listen(sock.Fd, 0) < 0)
        return TError(EError::Unknown, errno, "listen()");

    if (sock.Fd == PORTO_SK_FD)
        sock.SetFd = -1;
    else if (dup2(sock.Fd, PORTO_SK_FD) != PORTO_SK_FD)
        return TError(EError::Unknown, errno, "dup2()");

    return TError::Success();
}

static bool AnotherInstanceRunning(const std::string &path) {
    int fd;

    if (dup2(PORTO_SK_FD, PORTO_SK_FD) == PORTO_SK_FD)
        return false;

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

static int ReapSpawner(int fd) {
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
        EventQueue->Add(0, e);
    }

    return 0;
}

static int SlaveRpc() {
    TRpcWorker worker(config().daemon().workers());
    int ret = 0;
    std::map<int, std::shared_ptr<TClient>> clients;
    bool accept_paused = false;
    TError error;

    auto AcceptSource = std::make_shared<TEpollSource>(PORTO_SK_FD);
    error = EpollLoop->AddSource(AcceptSource);
    if (error) {
        L_ERR() << "Can't add RPC server fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    auto MasterSource = std::make_shared<TEpollSource>(REAP_EVT_FD);
    error = EpollLoop->AddSource(MasterSource);
    if (error && !failsafe) {
        L_ERR() << "Can't add master fd to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    /* Don't disturb threads. Deliver signals via signalfd. */
    int sigFd = SignalFd();

    auto sigSource = std::make_shared<TEpollSource>(sigFd);
    error = EpollLoop->AddSource(sigSource);
    if (error) {
        L_ERR() << "Can't add sigSource to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<struct epoll_event> events;

    worker.Start();
    EventQueue->Start();

    bool discardState = false;
    while (true) {
        if (accept_paused && clients.size() * 4 / 3 < config().daemon().max_clients()) {
            L_WRN() << "Resume accepting connections" << std::endl;
            error = EpollLoop->AddSource(AcceptSource);
            if (error) {
                L_ERR() << "Can't add RPC server fd to epoll: " << error << std::endl;
                ret = EXIT_FAILURE;
                goto exit;
            }
            accept_paused = false;
        }

        error = EpollLoop->GetEvents(events, -1);
        if (error) {
            L_ERR() << "slave: epoll error " << error << std::endl;
            ret = EXIT_FAILURE;
            goto exit;
        }

        if (!failsafe) {
            ret = ReapSpawner(REAP_EVT_FD);
            if (ret)
                goto exit;
        }

        for (auto ev : events) {
            auto source = EpollLoop->GetSource(ev.data.fd);
            if (!source)
                continue;

            if (source->Fd == sigFd) {
                struct signalfd_siginfo sigInfo;

                if (read(sigFd, &sigInfo, sizeof sigInfo) != sizeof sigInfo) {
                    L_ERR() << "SignalFd read failed" << std::endl;
                    continue;
                }

                switch (sigInfo.ssi_signo) {
                    case SIGINT:
                        discardState = true;
                        ret = -SIGTERM;
                        goto exit;
                    case SIGTERM:
                        ret = -SIGTERM;
                        goto exit;
                    case SIGHUP:
                        L_EVT() << "Updating" << std::endl;
                        ret = -SIGHUP;
                        goto exit;
                    case SIGUSR1:
                        DaemonOpenLog(false);
                        break;
                    case SIGUSR2:
                        DumpMallocInfo();
                        break;
                    case SIGCHLD:
                        break;
                    default:
                        L_WRN() << "Unexpected signal: " << sigInfo.ssi_signo << std::endl;
                        break;
                }
            } else if (source->Fd == PORTO_SK_FD) {
                if (!accept_paused && clients.size() >= config().daemon().max_clients()) {
                    L_WRN() << "Pause accepting connections" << std::endl;
                    EpollLoop->RemoveSource(AcceptSource->Fd);
                    accept_paused = true;
                    continue;
                }

                auto client = std::make_shared<TClient>();
                error = client->AcceptConnection(PORTO_SK_FD);
                if (!error)
                    error = EpollLoop->AddSource(client);
                if (!error)
                    clients[client->Fd] = client;
                if (error)
                    L() << "Drop client: " << error << std::endl;
            } else if (source->Fd == REAP_EVT_FD) {
                // we handled all events from the master before events
                // from the clients (so clients see updated view of the
                // world as soon as possible)
                continue;
            } else if (source->Flags & EPOLL_EVENT_OOM) {
                auto container = source->Container.lock();

                // we don't want any repeated events from OOM fd
                EpollLoop->StopInput(source->Fd);

                if (container) {
                    TEvent e(EEventType::OOM, container);
                    e.OOM.Fd = source->Fd;
                    EventQueue->Add(0, e);
                }

            } else if (clients.find(source->Fd) != clients.end()) {
                auto client = clients[source->Fd];

                if (ev.events & EPOLLIN) {
                    TRequest req {client};
                    error = client->ReadRequest(req.Request);

                    if (!error) {
                        error = client->IdentifyClient(false);
                        if (!error) {
                            Statistics->RequestsQueued++;
                            worker.Push(req);
                        }
                    }
                }

                if (ev.events & EPOLLOUT)
                    error = client->SendResponse(false);

                if ((ev.events & EPOLLHUP) || (ev.events & EPOLLERR) ||
                        (error && error.GetError() != EError::Queued)) {
                    clients.erase(source->Fd);
                    client->CloseConnection();
                }
            } else {
                L_WRN() << "Unknown event " << source->Fd << std::endl;
                EpollLoop->RemoveSource(source->Fd);
            }
        }
    }

exit:
    EventQueue->Stop();
    worker.Stop();

    for (auto c : clients)
        c.second->CloseConnection();
    clients.clear();

    if (discardState) {

        error = ContainersKV.UmountAll();
        if (error)
            L_ERR() << "Can't destroy key-value storage: " << error << std::endl;

        error = VolumesKV.UmountAll();
        if (error)
            L_ERR() << "Can't destroy volume key-value storage: " << error << std::endl;
    }

    return ret;
}

static void KvDump() {
    TLogger::OpenLog(true, "", 0);
    TKeyValue::DumpAll(TPath(config().keyval().file().path()));
    TKeyValue::DumpAll(TPath(config().volumes().keyval().file().path()));
}

static TError TuneLimits() {
    struct rlimit rlim;

    /*
     * two FDs for each container: OOM event and netlink
     * one for each client
     * plus some extra
     */
    int maxFd = config().container().max_total() * 2 +
                config().daemon().max_clients() + 1000;

    rlim.rlim_max = maxFd;
    rlim.rlim_cur = maxFd;

    int ret = setrlimit(RLIMIT_NOFILE, &rlim);
    if (ret)
        return TError(EError::Unknown, errno, "sertlimit");

    return TError::Success();
}

static TError CreateRootContainer() {
    TError error;

    error = TContainer::Create(ROOT_CONTAINER, RootContainer);
    if (error)
        return error;

    PORTO_ASSERT(RootContainer->IsRoot());

    RootContainer->Isolate = false;

    error = RootContainer->Start();
    if (error)
        return error;

    error = ContainerIdMap.GetAt(DEFAULT_TC_MINOR);
    if (error)
        return error;

    error = ContainerIdMap.GetAt(LEGACY_CONTAINER_ID);
    if (error)
        return error;

    //ScheduleLogRotatation();

    return TError::Success();
}

static void RestoreContainers() {
    std::list<TKeyValue> nodes;

    TError error = TKeyValue::ListAll(ContainersKV, nodes);
    if (error)
        FatalError("Cannot list container kv", error);

    for (auto node = nodes.begin(); node != nodes.end(); ) {
        error = node->Load();
        if (!error) {
            if (!node->Has(P_RAW_ID))
                error = TError(EError::Unknown, "id not found");
            if (!node->Has(P_RAW_NAME))
                error = TError(EError::Unknown, "name not found");
        }
        if (error) {
            L_ERR() << "Cannot load " << node->Path << ": " << error << std::endl;
            (void)node->Path.Unlink();
            node = nodes.erase(node);
            continue;
        }
        /* key for sorting */
        node->Name = node->Get(P_RAW_NAME);
        ++node;
    }

    nodes.sort();

    for (auto &node : nodes) {
        if (node.Name[0] == '/')
            continue;

        std::shared_ptr<TContainer> ct;
        error = TContainer::Restore(node, ct);
        if (error) {
            L_ERR() << "Cannot restore " << node.Name << ": " << error << std::endl;
            Statistics->RestoreFailed++;
            node.Path.Unlink();
            continue;
        }
    }
}

static void CleanupCgroups() {
    TError error;
    int pass = 0;
    bool retry;

again:
    retry = false;

    /* freezer must be first */
    for (auto hy: Hierarchies) {
        std::vector<TCgroup> cgroups;

        error = hy->RootCgroup().ChildsAll(cgroups);
        if (error)
            L_ERR() << "Cannot dump porto " << hy->Type << " cgroups : "
                    << error << std::endl;

        for (auto cg = cgroups.rbegin(); cg != cgroups.rend(); ++cg) {
            if (!StringStartsWith(cg->Name, PORTO_CGROUP_PREFIX))
                continue;

            if (cg->Name == PORTO_DAEMON_CGROUP &&
                    (hy->Controllers & (CGROUP_MEMORY | CGROUP_CPUACCT)))
                continue;

            if (cg->Name == PORTO_HELPERS_CGROUP &&
                    (hy->Controllers & CGROUP_MEMORY))
                continue;

            bool found = false;
            for (auto &it: Containers) {
                if (it.second->State != EContainerState::Stopped &&
                        it.second->GetCgroup(*hy) == *cg) {
                    found = true;
                    break;
                }
            }
            if (found)
                continue;

            if (!cg->IsEmpty())
                (void)cg->KillAll(9);

            if (hy == &FreezerSubsystem && FreezerSubsystem.IsFrozen(*cg)) {
                (void)FreezerSubsystem.Thaw(*cg, false);
                if (FreezerSubsystem.IsParentFreezing(*cg)) {
                    retry = true;
                    continue;
                }
            }

            (void)cg->Remove();
        }
    }

    if (retry && pass++ < 3)
        goto again;
}

static void CleanupTempdir() {
    TPath temp(config().container().tmp_dir());
    std::vector<std::string> list;
    TError error;

    error = temp.ReadDirectory(list);
    if (error)
        L_ERR() << "Cannot list temp dir: " << error << std::endl;

    for (auto &name: list) {
        auto it = Containers.find(name);
        if (it != Containers.end() &&
                it->second->State != EContainerState::Stopped)
            continue;
        TPath path = temp / name;
        error = path.RemoveAll();
        if (error)
            L_WRN() << "Cannot remove " << path << ": " << error << std::endl;
    }
}

static void DestroyWeakContainers() {
    for (auto it: Containers) {
        auto ct = it.second;
        if (ct->IsWeak)
            ct->Destroy();
    }
}

static void DestroyContainers() {
    for (auto it = Containers.rbegin(); it != Containers.rend(); ) {
        auto ct = it->second;
        ++it;
        TError error = ct->Destroy();
        if (error)
            L_ERR() << "Cannot destroy container " << ct->Name << ": " << error << std::endl;
    }
}

static int SlaveMain() {
    TError error;
    int ret;

    SetDieOnParentExit(SIGKILL);

    if (failsafe)
        AllocStatistics();

    Statistics->SlaveStarted = GetCurrentTimeMs();
    Statistics->ContainersCount = 0;
    Statistics->ClientsCount = 0;
    Statistics->VolumesCount = 0;
    Statistics->RequestsQueued = 0;

    ret = DaemonPrepare(false);
    if (ret)
        return ret;

    L_SYS() << "Previous version: " << PreviousVersion << std::endl;

    error = TuneLimits();
    if (error)
        FatalError("Cannot set correct limits", error);

    error = CreatePortoSocket();
    if (error)
        FatalError("Cannot create porto socket", error);

    if (fcntl(PORTO_SK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR() << "Can't set close-on-exec flag on PORTO_SK_FD: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
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

    error = SetOomScoreAdj(0);
    if (error)
        FatalError("Can't adjust OOM score", error);

    error = InitializeCgroups();
    if (error)
        FatalError("Cannot initalize cgroups", error);

    error = InitializeDaemonCgroups();
    if (error)
        FatalError("Cannot initalize daemon cgroups", error);

    TNetwork::InitializeConfig();
    InitContainerProperties();

    ContainersKV = TPath(config().keyval().file().path());
    error = TKeyValue::Mount(ContainersKV);
    if (error)
        FatalError("Cannot mount containers keyvalue", error);

    VolumesKV = TPath(config().volumes().keyval().file().path());
    error = TKeyValue::Mount(VolumesKV);
    if (error)
        FatalError("Cannot mount volumes keyvalue", error);

    EpollLoop = std::unique_ptr<TEpollLoop>(new TEpollLoop());
    EventQueue = std::unique_ptr<TEventQueue>(new TEventQueue());

    error = EpollLoop->Create();
    if (error)
        FatalError("Cannot initialize epoll", error);

    TPath tmp_dir(config().container().tmp_dir());
    if (!tmp_dir.IsDirectoryFollow()) {
        (void)tmp_dir.Unlink();
        error = tmp_dir.MkdirAll(0755);
        if (error)
            FatalError("Cannot create tmp_dir", error);
    }

    SystemClient.StartRequest();

    error = CreateRootContainer();
    if (error)
        FatalError("Cannot create root container", error);

    RestoreContainers();

    TVolume::RestoreAll();

    DestroyWeakContainers();

    SystemClient.FinishRequest();

    L() << "Remove cgroup leftovers..." << std::endl;
    CleanupCgroups();

    L() << "Cleanup temp dir..." << std::endl;
    CleanupTempdir();

    L() << "Done restoring" << std::endl;

    ret = SlaveRpc();
    L_SYS() << "Shutting down..." << std::endl;

    DaemonShutdown(false, ret);
    //FIXME ret >= 0 -> destroy kv storage? why???

    DestroyContainers();

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

static void UpdateQueueSize(std::map<int,int> &exited) {
    Statistics->QueuedStatuses = exited.size();
}

static int ReapDead(int fd, std::map<int,int> &exited, int slavePid, int &slaveStatus) {
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

static int SpawnSlave(std::shared_ptr<TEpollLoop> loop, std::map<int,int> &exited) {
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

    auto AckSource = std::make_shared<TEpollSource>(ackfd[0]);

    int sigFd = SignalFd();

    auto sigSource = std::make_shared<TEpollSource>(sigFd);

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
        close(sigFd);

        _exit(SlaveMain());
    }

    close(evtfd[0]);
    close(ackfd[1]);

    L_SYS() << "Spawned slave " << slavePid << std::endl;
    Statistics->Spawned++;

    for (auto &pair : exited)
        DeliverPidStatus(evtfd[1], pair.first, pair.second, exited.size());

    UpdateQueueSize(exited);

    error = loop->AddSource(AckSource);
    if (error) {
        L_ERR() << "Can't add ackfd[0] to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = loop->AddSource(sigSource);
    if (error) {
        L_ERR() << "Can't add sigSource to epoll: " << error << std::endl;
        return EXIT_FAILURE;
    }

    while (true) {
        std::vector<struct epoll_event> events;

        error = loop->GetEvents(events, -1);
        if (error) {
            L_ERR() << "master: epoll error " << error << std::endl;
            return EXIT_FAILURE;
        }

        struct signalfd_siginfo sigInfo;

        while (read(sigFd, &sigInfo, sizeof sigInfo) == sizeof sigInfo) {
            int s = sigInfo.ssi_signo;

            switch (s) {
            case SIGINT:
            case SIGTERM: {
                if (kill(slavePid, s) < 0)
                    L_ERR() << "Can't send " << s << " to slave" << std::endl;

                L() << "Waiting for slave to exit..." << std::endl;
                uint64_t deadline = GetCurrentTimeMs() + 1000;
                do {
                    if (waitpid(slavePid, nullptr, WNOHANG) == slavePid)
                        break;
                } while (!WaitDeadline(deadline));

                ret = -s;
                goto exit;
            }
            case SIGUSR1:
                DaemonOpenLog(true);
                break;
            case SIGUSR2:
                DumpMallocInfo();

                L() << "Statuses:" << std::endl;
                for (auto pair : exited)
                    L() << pair.first << "=" << pair.second << std::endl;;

                break;
            case SIGHUP:
            {
                int ret = DaemonSyncConfig(true);
                if (ret)
                    return ret;

                L_SYS() << "Updating" << std::endl;

                const char *stdlogArg = nullptr;
                if (stdlog)
                    stdlogArg = "--stdlog";

                if (kill(slavePid, SIGHUP) < 0) {
                    L_ERR() << "Can't send SIGHUP to slave: " << strerror(errno) << std::endl;
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
            }
            default:
                /* Ignore other signals */
                break;
            }
        }

        for (auto ev : events) {
            auto source = loop->GetSource(ev.data.fd);
            if (!source)
                continue;


            if (source->Fd == sigFd) {

            } else if (source->Fd == ackfd[0]) {
                if (!ReceiveAcks(ackfd[0], exited)) {
                    ret = EXIT_FAILURE;
                    goto exit;
                }
            } else {
                L_WRN() << "Unknown event " << source->Fd << std::endl;
                loop->RemoveSource(source->Fd);
            }
        }

        int status;
        if (ReapDead(evtfd[1], exited, slavePid, status)) {
            L_SYS() << "Slave exited with " << status << std::endl;
            ret = EXIT_SUCCESS;
            goto exit;
        }
    }

exit:
    loop->RemoveSource(sigFd);
    close(sigFd);

    loop->RemoveSource(AckSource->Fd);

    close(evtfd[0]);
    close(evtfd[1]);

    close(ackfd[0]);
    close(ackfd[1]);

    return ret;
}

static int MasterMain(bool respawn) {
    Statistics->MasterStarted = GetCurrentTimeMs();

    int ret = DaemonPrepare(true);
    if (ret)
        return ret;

    TPath pathVer(PORTO_VERSION_FILE);

    if (pathVer.ReadAll(PreviousVersion)) {
        (void)pathVer.Mkfile(0644);
        PreviousVersion = "";
    } else {
        if (PreviousVersion[0] == 'v')
            PreviousVersion = PreviousVersion.substr(1);
        L_SYS() << "Previous version: " << PreviousVersion << std::endl;
    }

    if (pathVer.WriteAll(PORTO_VERSION))
        L_ERR() << "Can't update current version" << std::endl;

    std::shared_ptr<TEpollLoop> ELoop = std::make_shared<TEpollLoop>();
    TError error = ELoop->Create();
    if (error)
        return EXIT_FAILURE;

    // We want propogate mounts into containers
    error = TPath("/").Remount(MS_SHARED | MS_REC);
    if (error) {
        L_ERR() << "Can't remount / recursively as shared" << error << std::endl;
        return EXIT_FAILURE;
    }

#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TError error(EError::Unknown, errno, "prctl(PR_SET_CHILD_SUBREAPER)");
        L_ERR() << "Can't set myself as a subreaper, make sure kernel version is at least 3.4: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = SetOomScoreAdj(-1000);
    if (error)
        L_ERR() << "Can't adjust OOM score: " << error << std::endl;

    error = CreatePortoSocket();
    if (error) {
        L_ERR() << "Cannot create porto socket: " << error << std::endl;
        return EXIT_FAILURE;
    }

    std::map<int,int> exited;

    while (true) {
        uint64_t started = GetCurrentTimeMs();
        uint64_t next = started + config().container().respawn_delay_ms();
        ret = SpawnSlave(ELoop, exited);
        L() << "Returned " << ret << std::endl;
        if (next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);

        if (slavePid) {
            (void)kill(slavePid, SIGKILL);
            Reap(slavePid);
        }

        if (ret < 0)
            break;
        if (!respawn)
            break;

        PreviousVersion = PORTO_VERSION;
    }

    error = TPath(PORTO_SOCKET_PATH).Unlink();
    if (error)
        L_ERR() << "Cannot unlink socket file: " << error << std::endl;

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
    bool kvDump = false;
    int argn;

    for (argn = 1; argn < argc; argn++) {
        std::string arg(argv[argn]);

        if (arg == "-v" || arg == "--version") {
            std::cout << PORTO_VERSION << " " << PORTO_REVISION << std::endl;
            return EXIT_SUCCESS;
        } else if (arg == "--kv-dump") {
            kvDump = true;
        } else if (arg == "--slave") {
            slaveMode = true;
        } else if (arg == "--stdlog") {
            stdlog = true;
        } else if (arg == "--verbose") {
            Verbose = true;
        } else if (arg == "--norespawn") {
            respawn = false;
        } else if (arg == "--failsafe") {
            failsafe = true;
        } else if (arg == "-t") {
            if (argn + 1 >= argc)
                return EXIT_FAILURE;
            return config.Test(argv[argn + 1]);
        } else {
            std::cerr << "Unknown option " << arg << std::endl;
            return EXIT_FAILURE;
        }
    }

    if (getuid() != 0) {
        std::cerr << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    if (RunningInContainer()) {
        std::cerr << "Can't start in container" << std::endl;
        return EXIT_FAILURE;
    }

    CatchFatalSignals();

    AllocStatistics();

    config.Load();

    if (kvDump) {
        KvDump();
        return EXIT_SUCCESS;
    }

    if (!slaveMode && AnotherInstanceRunning(PORTO_SOCKET_PATH)) {
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
        L_ERR() << "EXCEPTION: " << s << std::endl;
        Crash();
    } catch (const char *s) {
        L_ERR() << "EXCEPTION: " << s << std::endl;
        Crash();
    } catch (const std::exception &exc) {
        L_ERR() << "EXCEPTION: " << exc.what() << std::endl;
        Crash();
    } catch (...) {
        L_ERR() << "EXCEPTION: uncaught exception!" << std::endl;
        Crash();
    }
}
