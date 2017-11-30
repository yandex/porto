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
#include "storage.hpp"
#include "helpers.hpp"
#include "core.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/signal.hpp"
#include "util/unix.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/worker.hpp"
#include "property.hpp"
#include "portod.hpp"
#include "libporto.hpp"

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

static TPidFile MasterPidFile(PORTO_MASTER_PIDFILE, PORTOD_MASTER_NAME, "portod");
static TPidFile PortodPidFile(PORTO_PIDFILE, PORTOD_NAME, "portod-slave");

std::unique_ptr<TEpollLoop> EpollLoop;
std::unique_ptr<TEventQueue> EventQueue;

static pid_t PortodPid;
static bool respawnPortod = true;
static bool discardState = false;

static void FatalError(const std::string &text, TError &error) {
    L_ERR("{}: {}", text, error);
    _exit(EXIT_FAILURE);
}

static void AllocStatistics() {
    Statistics = (TStatistics *)mmap(nullptr, sizeof(*Statistics),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    PORTO_ASSERT(Statistics != nullptr);
}

static void DaemonShutdown(bool master, int code) {
    L_SYS("Stopped {}", code);

    if (master)
        MasterPidFile.Remove();
    else
        PortodPidFile.Remove();

    if (code < 0) {
        int sig = -code;
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

        auto time = request.Client->RequestTimeMs;
        if (time > 1000)
            Statistics->RequestsLonger1s++;
        if (time > 3000)
            Statistics->RequestsLonger3s++;
        if (time > 30000)
            Statistics->RequestsLonger30s++;
        if (time > 300000)
            Statistics->RequestsLonger5m++;

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
            L_SYS("Reuse porto socket: inode {} : {} "
                  "age {} : {}",
                  fd_stat.st_ino, sk_stat.st_ino,
                  now - fd_stat.st_ctime, now - sk_stat.st_ctime);
            return TError::Success();
        }

        L_WRN("Unlinked porto socket. Recreating...");
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

void AckExitStatus(int pid) {
    if (!pid)
        return;

    L_DBG("Acknowledge exit status for {}", pid);
    int ret = write(REAP_ACK_FD, &pid, sizeof(pid));
    if (ret != sizeof(pid)) {
        TError error(EError::Unknown, errno, "write(): returned " + std::to_string(ret));
        L_ERR("Can't acknowledge exit status for {}: {}", pid, error);
        Crash();
    }
}

static int RecvExitEvents(int fd) {
    struct pollfd fds[1];
    int nr = 1000;

    fds[0].fd = fd;
    fds[0].events = POLLIN | POLLHUP;

    while (nr--) {
        int ret = poll(fds, 1, 0);
        if (ret < 0) {
            L_ERR("poll() error: {}", strerror(errno));
            return ret;
        }

        if (!fds[0].revents || (fds[0].revents & POLLHUP))
            return 0;

        int pid, status;
        if (read(fd, &pid, sizeof(pid)) != sizeof(pid)) {
            L_ERR("read(pid): {}", strerror(errno));
            return 0;
        }
retry:
        if (read(fd, &status, sizeof(status)) != sizeof(status)) {
            if (errno == EAGAIN)
                goto retry;
            L_ERR("read(status): {}", strerror(errno));
            return 0;
        }

        TEvent e(EEventType::Exit);
        e.Exit.Pid = pid;
        e.Exit.Status = status;
        EventQueue->Add(0, e);
    }

    return 0;
}

static std::map<int, std::shared_ptr<TClient>> Clients;

static TError DropIdleClient(std::shared_ptr<TContainer> from = nullptr) {
    uint64_t idle = config().daemon().client_idle_timeout() * 1000;
    uint64_t now = GetCurrentTimeMs();
    std::shared_ptr<TClient> victim;

    for (auto &it: Clients) {
        auto &client = it.second;

        if (client->Processing)
            continue;

        if (from && client->ClientContainer != from)
            continue;

        if (now - client->ActivityTimeMs > idle) {
            victim = client;
            idle = now - client->ActivityTimeMs;
        }
    }

    if (!victim)
        return TError(EError::ResourceNotAvailable,
                      "All client slots are active: " +
                      (from ? from->Name : "globally"));

    L_ACT("Drop client {} idle for {} ms", victim->Id, idle);
    Clients.erase(victim->Fd);
    victim->CloseConnection();
    return TError::Success();
}

static TError AcceptConnection(int listenFd) {
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;
    TError error;
    int clientFd;

    peer_addr_size = sizeof(struct sockaddr_un);
    clientFd = accept4(listenFd, (struct sockaddr *) &peer_addr,
                       &peer_addr_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (clientFd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return TError::Success(); /* client already gone */
        return TError(EError::Unknown, errno, "accept4()");
    }

    auto client = std::make_shared<TClient>(clientFd);
    error = client->IdentifyClient(true);
    if (error)
        return error;

    int max_clients = config().daemon().max_clients_in_container();
    if (client->IsSuperUser())
        max_clients += NR_SUPERUSER_CLIENTS;

    if (client->ClientContainer->ClientsCount > max_clients) {
        error = DropIdleClient(client->ClientContainer);
        if (error)
            return error;
    }

    max_clients = config().daemon().max_clients();
    if (client->IsSuperUser())
        max_clients += NR_SUPERUSER_CLIENTS;

    if (Statistics->ClientsCount > max_clients) {
        error = DropIdleClient();
        if (error)
            return error;
    }

    error = EpollLoop->AddSource(client);
    if (error)
        return error;

    client->InEpoll = true; /* FIXME cleanup this crap */
    Clients[client->Fd] = client;

    return TError::Success();
}

static int Rpc() {
    TRpcWorker worker(config().daemon().workers());
    int ret = 0;
    TError error;

    auto AcceptSource = std::make_shared<TEpollSource>(PORTO_SK_FD);
    error = EpollLoop->AddSource(AcceptSource);
    if (error) {
        L_ERR("Can't add RPC server fd to epoll: {}", error);
        return EXIT_FAILURE;
    }

    auto MasterSource = std::make_shared<TEpollSource>(REAP_EVT_FD);
    error = EpollLoop->AddSource(MasterSource);
    if (error) {
        L_ERR("Can't add master fd to epoll: {}", error);
        return EXIT_FAILURE;
    }

    /* Don't disturb threads. Deliver signals via signalfd. */
    int sigFd = SignalFd();

    auto sigSource = std::make_shared<TEpollSource>(sigFd);
    error = EpollLoop->AddSource(sigSource);
    if (error) {
        L_ERR("Can't add sigSource to epoll: {}", error);
        return EXIT_FAILURE;
    }

    std::vector<struct epoll_event> events;

    worker.Start();
    EventQueue->Start();

    if (config().daemon().log_rotate_ms()) {
        TEvent ev(EEventType::RotateLogs);
        EventQueue->Add(config().daemon().log_rotate_ms(), ev);
    }

    while (true) {
        error = EpollLoop->GetEvents(events, -1);
        if (error) {
            L_ERR("epoll error {}", error);
            ret = EXIT_FAILURE;
            goto exit;
        }

        ret = RecvExitEvents(REAP_EVT_FD);
        if (ret)
            goto exit;

        for (auto ev : events) {
            auto source = EpollLoop->GetSource(ev.data.fd);
            if (!source)
                continue;

            if (source->Fd == sigFd) {
                struct signalfd_siginfo sigInfo;

                if (read(sigFd, &sigInfo, sizeof sigInfo) != sizeof sigInfo) {
                    L_ERR("SignalFd read failed");
                    continue;
                }

                switch (sigInfo.ssi_signo) {
                    case SIGINT:
                        discardState = true;
                        ret = -SIGINT;
                        goto exit;
                    case SIGTERM:
                        ret = -SIGTERM;
                        goto exit;
                    case SIGHUP:
                        L_EVT("Updating");
                        ret = -SIGHUP;
                        goto exit;
                    case SIGUSR1:
                        OpenLog(PORTO_LOG);
                        break;
                    case SIGUSR2:
                        DumpMallocInfo();
                        TContainer::DumpLocks();
                        break;
                    case SIGCHLD:
                        if (!TTask::Deliver(sigInfo.ssi_pid, sigInfo.ssi_status)) {
                            TEvent e(EEventType::ChildExit);
                            e.Exit.Pid = sigInfo.ssi_pid;
                            e.Exit.Status = sigInfo.ssi_status;
                            EventQueue->Add(0, e);
                        }
                        break;
                    default:
                        L_WRN("Unexpected signal: {}", sigInfo.ssi_signo);
                        break;
                }
            } else if (source->Fd == PORTO_SK_FD) {
                error = AcceptConnection(source->Fd);
                if (error)
                    L("Cannot accept connection: {}", error);
            } else if (source->Fd == REAP_EVT_FD) {
                // we handled all events from the master before events
                // from the clients (so clients see updated view of the
                // world as soon as possible)
                continue;
            } else if (source->Flags & EPOLL_EVENT_OOM) {
                auto container = source->Container.lock();

                if (!container) {
                    L_WRN("Container not found for OOM fd {}", source->Fd);
                    EpollLoop->StopInput(source->Fd);
                } else if (container->RecvOomEvents() && container->OomIsFatal) {
                    EpollLoop->StopInput(source->Fd);
                    TEvent e(EEventType::OOM, container);
                    EventQueue->Add(0, e);
                }

            } else if (Clients.find(source->Fd) != Clients.end()) {
                auto client = Clients[source->Fd];

                if (ev.events & EPOLLIN) {
                    TRequest req;

                    req.Client = client;
                    error = client->ReadRequest(req.Request);

                    if (!error) {
                        error = client->IdentifyClient(false);
                        if (!error) {
                            client->ClientContainer->ContainerRequests++;
                            Statistics->RequestsQueued++;
                            worker.Push(req);
                        }
                    }
                }

                if (ev.events & EPOLLOUT)
                    error = client->SendResponse(false);

                if ((ev.events & EPOLLHUP) || (ev.events & EPOLLERR) ||
                        (error && error.GetError() != EError::Queued)) {
                    Clients.erase(source->Fd);
                    client->CloseConnection();
                }
            } else {
                L_WRN("Unknown event {}", source->Fd);
                EpollLoop->RemoveSource(source->Fd);
            }
        }
    }

exit:
    EventQueue->Stop();
    worker.Stop();

    for (auto c : Clients)
        c.second->CloseConnection();
    Clients.clear();

    return ret;
}

static TError TuneLimits() {
    struct rlimit rlim;

    /*
     * two FDs for each container: OOM event and netlink
     * ten for each thread
     * one for each client
     * plus some extra
     */
    int maxFd = config().container().max_total() * 2 +
                NR_SUPERUSER_CONTAINERS * 2 +
                config().daemon().workers() * 10 +
                config().daemon().max_clients() +
                NR_SUPERUSER_CLIENTS +
                1000;

    L_SYS("Estimated portod file descriptor limit: {}", maxFd);

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

    PORTO_ASSERT(RootContainer->Id == ROOT_CONTAINER_ID);
    PORTO_ASSERT(RootContainer->IsRoot());

    RootContainer->Isolate = false;

    error = StringToStringMap(config().container().default_ulimit(),
                              RootContainer->Ulimit);
    if (error)
        return error;

    if (!RootContainer->Ulimit.count("nproc")) {
        uint64_t pids, threads, lim;
        std::string str;

        if (!GetSysctl("kernel.pid_max", str) &&
                !StringToUint64(str, pids) &&
                !GetSysctl("kernel.threads-max", str) &&
                !StringToUint64(str, threads)) {
            lim = std::min(pids, threads) / 2;
            L_SYS("Default nproc ulimit: {}", lim);
            RootContainer->Ulimit["nproc"] = fmt::format("{} {}", lim, lim);
        }
    }

    error = SystemClient.LockContainer(RootContainer);
    if (error)
        return error;

    error = RootContainer->Start();
    if (error)
        return error;

    error = ContainerIdMap.GetAt(DEFAULT_TC_MINOR);
    if (error)
        return error;

    error = ContainerIdMap.GetAt(LEGACY_CONTAINER_ID);
    if (error)
        return error;

    SystemClient.ReleaseContainer();

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
            L_ERR("Cannot load {}: {}", node->Path, error);
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
            L_ERR("Cannot restore {}: {}", node.Name, error);
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
            L_ERR("Cannot dump porto {} cgroups : {}", hy->Type, error);

        for (auto cg = cgroups.rbegin(); cg != cgroups.rend(); ++cg) {
            if (!StringStartsWith(cg->Name, PORTO_CGROUP_PREFIX))
                continue;

            if (cg->Name == PORTO_DAEMON_CGROUP &&
                    (hy->Controllers & (CGROUP_MEMORY | CGROUP_CPUACCT)))
                continue;

            if (cg->Name == PORTO_HELPERS_CGROUP &&
                    (hy->Controllers & CGROUP_MEMORY))
                continue;

            if (cg->Name == PORTO_CGROUP_PREFIX &&
                    (hy->Controllers & CGROUP_FREEZER))
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
    TPath temp(PORTO_WORKDIR);
    std::vector<std::string> list;
    TError error;

    error = temp.ReadDirectory(list);
    if (error)
        L_ERR("Cannot list temp dir: {}", error);

    for (auto &name: list) {
        auto it = Containers.find(name);
        if (it != Containers.end() &&
                it->second->State != EContainerState::Stopped)
            continue;
        TPath path = temp / name;
        error = RemoveRecursive(path);
        if (error) {
            L_WRN("Cannot remove {}: {}", path, error);

            error = path.RemoveAll();
            if (error)
                L_WRN("Cannot delete {}: {}", path, error);
        }
    }
}

static void DestroyContainers(bool weak) {
    /* leaves first */
    for (auto &ct: RootContainer->Subtree()) {
        if (ct->IsRoot() || (weak && !ct->IsWeak))
            continue;

        TError error = SystemClient.LockContainer(ct);
        if (!error)
            error = ct->Destroy();

        if (error)
            L_ERR("Cannot destroy container {}: {}", ct->Name, error);
    }

    SystemClient.ReleaseContainer();
}

static int Portod() {
    TError error;

    SetDieOnParentExit(SIGKILL);

    Statistics->PortoStarted = GetCurrentTimeMs();
    Statistics->ContainersCount = 0;
    Statistics->ClientsCount = 0;
    Statistics->VolumesCount = 0;
    Statistics->RequestsQueued = 0;

    SetProcessName(PORTOD_NAME);

    OpenLog(PORTO_LOG);

    error = PortodPidFile.Save(getpid());
    if (error)
        FatalError("Cannot save pid", error);

    config.Load();
    InitPortoCgroups();
    InitCapabilities();
    InitIpcSysctl();
    TNetwork::InitializeConfig();

    L_SYS("Portod config:\n{}", config().DebugString());

    error = TuneLimits();
    if (error)
        FatalError("Cannot set correct limits", error);

    error = CreatePortoSocket();
    if (error)
        FatalError("Cannot create porto socket", error);

    if (fcntl(PORTO_SK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR("Can't set close-on-exec flag on PORTO_SK_FD: {}", strerror(errno));
        return EXIT_FAILURE;
    }

    if (fcntl(REAP_EVT_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR("Can't set close-on-exec flag on REAP_EVT_FD: {}", strerror(errno));
        return EXIT_FAILURE;
    }

    if (fcntl(REAP_ACK_FD, F_SETFD, FD_CLOEXEC) < 0) {
        L_ERR("Can't set close-on-exec flag on REAP_ACK_FD: {}", strerror(errno));
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

    InitContainerProperties();
    TStorage::Init();

    ContainersKV = TPath(PORTO_CONTAINERS_KV);
    error = TKeyValue::Mount(ContainersKV);
    if (error)
        FatalError("Cannot mount containers keyvalue", error);

    VolumesKV = TPath(PORTO_VOLUMES_KV);
    error = TKeyValue::Mount(VolumesKV);
    if (error)
        FatalError("Cannot mount volumes keyvalue", error);

    // We want propagate mounts into containers
    error = TPath("/").Remount(MS_SHARED | MS_REC);
    if (error)
        FatalError("Can't remount / recursively as shared", error);

    EpollLoop = std::unique_ptr<TEpollLoop>(new TEpollLoop());
    EventQueue = std::unique_ptr<TEventQueue>(new TEventQueue());

    error = EpollLoop->Create();
    if (error)
        FatalError("Cannot initialize epoll", error);

    TPath tmp_dir(PORTO_WORKDIR);
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

    SystemClient.ClientContainer = RootContainer;

    RestoreContainers();

    TContainer::SyncPropertiesAll();

    TVolume::RestoreAll();

    DestroyContainers(true);

    if (discardState) {
        discardState = false;
        L_ACT("Discard state...");
        DestroyContainers(false);
        TVolume::DestroyAll();
    }

    SystemClient.FinishRequest();

    L_ACT("Remove cgroup leftovers...");
    CleanupCgroups();

    L_ACT("Cleanup temp dir...");
    CleanupTempdir();

    L_SYS("Done restoring");

    int code = Rpc();
    L_SYS("Shutting down...");

    if (discardState) {
        discardState = false;

        L_ACT("Discard state...");

        SystemClient.LockContainer(RootContainer);

        error = RootContainer->Stop(0);
        if (error)
            L_ERR("Failed to stop root container and its children {}", error);

        SystemClient.ReleaseContainer();

        SystemClient.StartRequest();
        DestroyContainers(false);
        TVolume::DestroyAll();
        SystemClient.FinishRequest();

        SystemClient.LockContainer(RootContainer);

        error = RootContainer->Destroy();
        if (error)
            L_ERR("Cannot destroy root container{}", error);

        SystemClient.ReleaseContainer();

        RootContainer = nullptr;

        error = ContainersKV.UmountAll();
        if (error)
            L_ERR("Can't destroy key-value storage: {}", error);

        error = VolumesKV.UmountAll();
        if (error)
            L_ERR("Can't destroy volume key-value storage: {}", error);
    }

    DaemonShutdown(false, code);

    return code;
}

static TError DeliverPidStatus(int fd, int pid, int status, size_t queued) {
    L_EVT("Deliver {} status {} ({} queued)", pid, status, queued);

    if (write(fd, &pid, sizeof(pid)) < 0)
        return TError(EError::Unknown, "write(pid): ", errno);

    if (write(fd, &status, sizeof(status)) < 0)
        return TError(EError::Unknown, "write(status): ", errno);

    return TError::Success();
}

static void Reap(int pid) {
    (void)waitpid(pid, NULL, 0);
}

static void UpdateQueueSize(std::map<int,int> &exited) {
    Statistics->QueuedStatuses = exited.size();
}

static int ReapDead(int fd, std::map<int,int> &exited, int &portodStatus) {
    while (true) {
        siginfo_t info;

        if (waitpid(PortodPid, &portodStatus, WNOHANG) == PortodPid)
            return -1;

        info.si_pid = 0;

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

        if (info.si_pid == PortodPid) {
            portodStatus = status;
            Reap(info.si_pid);
            return -1;
        }

        if (exited.find(info.si_pid) != exited.end())
            break;

        exited[info.si_pid] = status;
        TError error = DeliverPidStatus(fd, info.si_pid, status, exited.size());
        if (error)
            L_WRN("Fail to deliver pid status to porto: {}", error);
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
            L_WRN("Got acknowledge for unknown pid {}", pid);
        } else {
            exited.erase(pid);
            Reap(pid);
            L_EVT("Got acknowledge for {} ({} queued)", pid, exited.size());
        }

        nr++;
    }

    UpdateQueueSize(exited);
    return nr;
}

static int UpgradeMaster() {
    L_SYS("Updating");

    if (kill(PortodPid, SIGHUP) < 0) {
        L_ERR("Cannot send SIGHUP to porto: {}", strerror(errno));
    } else {
        if (waitpid(PortodPid, NULL, 0) != PortodPid)
            L_ERR("Cannot wait for porto exit status: {}", strerror(errno));
    }

    std::vector<const char *> args = {PORTO_BINARY_PATH};
    if (StdLog)
        args.push_back("--stdlog");
    if (Debug)
        args.push_back("--debug");
    else if (Verbose)
        args.push_back("--verbose");
    args.push_back(nullptr);

    execvp(args[0], (char **)args.data());

    args[0] = program_invocation_name;
    execvp(args[0], (char **)args.data());

    args[0] = "portod";
    execvp(args[0], (char **)args.data());

    args[0] = "/usr/sbin/portod";
    execvp(args[0], (char **)args.data());

    std::cerr << "Cannot exec " << args[0] << ": " << strerror(errno) << std::endl;
    return EXIT_FAILURE;
}

static int SpawnPortod(std::shared_ptr<TEpollLoop> loop, std::map<int,int> &exited) {
    int evtfd[2];
    int ackfd[2];
    int ret = EXIT_FAILURE;
    TError error;

    PortodPid = 0;

    if (pipe2(evtfd, O_NONBLOCK | O_CLOEXEC) < 0) {
        L_ERR("pipe(): {}", strerror(errno));
        return EXIT_FAILURE;
    }

    if (pipe2(ackfd, O_NONBLOCK | O_CLOEXEC) < 0) {
        L_ERR("pipe(): {}", strerror(errno));
        return EXIT_FAILURE;
    }

    auto AckSource = std::make_shared<TEpollSource>(ackfd[0]);

    int sigFd = SignalFd();

    auto sigSource = std::make_shared<TEpollSource>(sigFd);

    PortodPid = fork();
    if (PortodPid < 0) {
        L_ERR("fork(): {}", strerror(errno));
        ret = EXIT_FAILURE;
        goto exit;
    } else if (PortodPid == 0) {
        close(evtfd[1]);
        close(ackfd[0]);
        loop->Destroy();
        (void)dup2(evtfd[0], REAP_EVT_FD);
        (void)dup2(ackfd[1], REAP_ACK_FD);
        close(evtfd[0]);
        close(ackfd[1]);
        close(sigFd);

        _exit(Portod());
    }

    close(evtfd[0]);
    close(ackfd[1]);

    L_SYS("Spawned portod {}", PortodPid);
    Statistics->Spawned++;

    for (auto &pair : exited)
        (void)DeliverPidStatus(evtfd[1], pair.first, pair.second, exited.size());

    UpdateQueueSize(exited);

    error = loop->AddSource(AckSource);
    if (error) {
        L_ERR("Can't add ackfd[0] to epoll: {}", error);
        return EXIT_FAILURE;
    }

    error = loop->AddSource(sigSource);
    if (error) {
        L_ERR("Can't add sigSource to epoll: {}", error);
        return EXIT_FAILURE;
    }

    while (true) {
        std::vector<struct epoll_event> events;

        error = loop->GetEvents(events, -1);
        if (error) {
            L_ERR("master: epoll error {}", error);
            return EXIT_FAILURE;
        }

        struct signalfd_siginfo sigInfo;

        while (read(sigFd, &sigInfo, sizeof sigInfo) == sizeof sigInfo) {
            int s = sigInfo.ssi_signo;

            switch (s) {
            case SIGINT:
            case SIGTERM: {
                if (kill(PortodPid, s) < 0)
                    L_ERR("Can't send {} to porto", s);

                L_SYS("Waiting for porto to exit...");
                uint64_t deadline = GetCurrentTimeMs() +
                                    config().daemon().portod_stop_timeout() * 1000;
                do {
                    if (waitpid(PortodPid, nullptr, WNOHANG) == PortodPid)
                        break;
                } while (!WaitDeadline(deadline, 1));

                ret = -s;
                goto exit;
            }
            case SIGUSR1:
                OpenLog(PORTO_LOG);
                break;
            case SIGUSR2:
                DumpMallocInfo();

                L("Statuses:");
                for (auto pair : exited)
                    L("{} = {}", pair.first, pair.second);

                break;
            case SIGHUP:
                return UpgradeMaster();
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
                L_WRN("Unknown event {}", source->Fd);
                loop->RemoveSource(source->Fd);
            }
        }

        int status;
        if (ReapDead(evtfd[1], exited, status)) {
            L_SYS("Portod exited with {}", status);
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

static int PortodMaster() {
    TError error;

    Statistics->MasterStarted = GetCurrentTimeMs();

    SetProcessName(PORTOD_MASTER_NAME);

    OpenLog(PORTO_LOG);

    error = MasterPidFile.Save(getpid());
    if (error)
        FatalError("Cannot save pid", error);

    TPath pathVer(PORTO_VERSION_FILE);

    if (pathVer.ReadAll(PreviousVersion)) {
        (void)pathVer.Mkfile(0644);
        PreviousVersion = "";
    } else {
        if (PreviousVersion[0] == 'v')
            PreviousVersion = PreviousVersion.substr(1);
    }

    if (pathVer.WriteAll(PORTO_VERSION))
        L_ERR("Can't update current version");

    TPath pathBin(PORTO_BINARY_PATH), prevBin;
    TPath procExe("/proc/self/exe"), thisBin;
    error = procExe.ReadLink(thisBin);
    if (error)
        FatalError("Cannot read /proc/self/exe", error);
    (void)pathBin.ReadLink(prevBin);

    if (prevBin != thisBin) {
        (void)pathBin.Unlink();
        error = pathBin.Symlink(thisBin);
        if (error)
            FatalError("Cannot update " + std::string(PORTO_BINARY_PATH), error);
    }

    L_SYS("{}", std::string(80, '-'));
    L_SYS("Started {} {} {} {}", PORTO_VERSION, PORTO_REVISION, GetPid(), thisBin);
    L_SYS("Previous version: {} {}", PreviousVersion, prevBin);

    std::shared_ptr<TEpollLoop> ELoop = std::make_shared<TEpollLoop>();
    error = ELoop->Create();
    if (error)
        return EXIT_FAILURE;

#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TError error(EError::Unknown, errno, "prctl(PR_SET_CHILD_SUBREAPER)");
        L_ERR("Can't set myself as a subreaper, make sure kernel version is at least 3.4: {}", error);
        return EXIT_FAILURE;
    }

    error = SetOomScoreAdj(-1000);
    if (error)
        L_ERR("Can't adjust OOM score: {}", error);

    error = CreatePortoSocket();
    if (error) {
        L_ERR("Cannot create porto socket: {}", error);
        return EXIT_FAILURE;
    }

    error = TCore::Register(thisBin);
    if (error) {
        L_ERR("Cannot setup core pattern: {}", error);
        return EXIT_FAILURE;
    }

    std::map<int,int> exited;
    int code;

    while (true) {
        uint64_t started = GetCurrentTimeMs();
        uint64_t next = started + config().container().respawn_delay_ms();
        code = SpawnPortod(ELoop, exited);
        L_SYS("Portod returned {}", code);
        if (next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);

        if (PortodPid) {
            (void)kill(PortodPid, SIGKILL);
            Reap(PortodPid);
        }

        if (code < 0 || !respawnPortod)
            break;

        PreviousVersion = PORTO_VERSION;
    }

    error = TCore::Unregister();
    if (error)
        L_ERR("Cannot revert core pattern: {}", error);

    error = TPath(PORTO_SOCKET_PATH).Unlink();
    if (error)
        L_ERR("Cannot unlink socket file: {}", error);

    DaemonShutdown(true, code);

    return code;
}

static void KvDump() {
    TKeyValue::DumpAll(PORTO_CONTAINERS_KV);
    TKeyValue::DumpAll(PORTO_VOLUMES_KV);
}

static void PrintVersion() {
    TPath thisBin, currBin;

    TPath("/proc/self/exe").ReadLink(thisBin);
    if (MasterPidFile.Load() || TPath("/proc/" +
                std::to_string(MasterPidFile.Pid) +"/exe").ReadLink(currBin))
        TPath(PORTO_BINARY_PATH).ReadLink(currBin);

    std::cout << "version: " << PORTO_VERSION << " " << PORTO_REVISION << " " << thisBin << std::endl;

    Porto::Connection conn;
    std::string ver, rev;
    if (!conn.GetVersion(ver, rev))
        std::cout << "running: " <<  ver + " " + rev << " " << currBin << std::endl;
}

static bool CheckPortoAlive() {
    Porto::Connection conn;
    if (conn.SetTimeout(1) != EError::Success)
        return false;
    std::string ver, rev;
    return !conn.GetVersion(ver, rev);
}

static bool RunningInContainer() {
    if (getpid() == 1)
        return getenv("container") != nullptr;
    std::string env;
    if (TPath("/proc/1/environ").ReadAll(env))
        return false;
    return StringStartsWith(env, "container=") ||
        env.find(std::string("\0container=", 11)) != std::string::npos;
}

static bool SanityCheck() {
    if (getuid() != 0) {
        std::cerr << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    if (!MasterPidFile.Load() && MasterPidFile.Pid != getpid() && CheckPortoAlive()) {
        std::cerr << "Another instance of portod is running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (RunningInContainer()) {
        std::cerr << "Cannot start in container" << std::endl;
        return EXIT_FAILURE;
    }

    if (CompareVersions(config().linux_version(), "3.18") < 0) {
        std::cerr << "Require Linux >= 3.18\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int PortodMain() {
    if (SanityCheck())
        return EXIT_FAILURE;

    CatchFatalSignals();

    AllocStatistics();

    (void)close(STDIN_FILENO);
    int null = open("/dev/null", O_RDWR);
    PORTO_ASSERT(null == STDIN_FILENO);

    if (!StdLog || fcntl(STDOUT_FILENO, F_GETFD) < 0) {
        int ret = dup2(null, STDOUT_FILENO);
        PORTO_ASSERT(ret == STDOUT_FILENO);
    }

    if (!StdLog || fcntl(STDERR_FILENO, F_GETFD) < 0) {
        int ret = dup2(null, STDERR_FILENO);
        PORTO_ASSERT(ret == STDERR_FILENO);
    }

    try {
        return PortodMaster();
    } catch (std::string s) {
        L_ERR("EXCEPTION: {}", s);
        Crash();
    } catch (const char *s) {
        L_ERR("EXCEPTION: {}", s);
        Crash();
    } catch (const std::exception &exc) {
        L_ERR("EXCEPTION: {}", exc.what());
        Crash();
    } catch (...) {
        L_ERR("EXCEPTION: uncaught exception!");
        Crash();
    }

    return EXIT_FAILURE;
}

static int StartPortod() {
    if (SanityCheck())
        return EXIT_FAILURE;

    pid_t pid = fork();
    if (pid < 0)
        return EXIT_FAILURE;

    if (!pid)
        return PortodMain();

    uint64_t deadline = GetCurrentTimeMs() + config().daemon().portod_start_timeout() * 1000;
    do {
        if (CheckPortoAlive())
            return EXIT_SUCCESS;
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            std::cerr << "portod exited: " << FormatExitStatus(status) << std::endl;
            return EXIT_FAILURE;
        }
    } while (!WaitDeadline(deadline));
    std::cerr << "start timeout exceeded" << std::endl;
    return EXIT_FAILURE;
}

static int StopPortod() {
    TError error;

    if (MasterPidFile.Load()) {
        std::cerr << "portod already stopped" << std::endl;
        return EXIT_SUCCESS;
    }

    pid_t pid = MasterPidFile.Pid;
    if (CheckPortoAlive()) {
        if (!kill(pid, SIGINT)) {
            uint64_t deadline = GetCurrentTimeMs() + config().daemon().portod_stop_timeout() * 1000;
            do {
                if (MasterPidFile.Load() || MasterPidFile.Pid != pid)
                    return EXIT_SUCCESS;
            } while (!WaitDeadline(deadline));
        } else if (errno != ESRCH) {
            std::cerr << "cannot stop portod: " << strerror(errno) << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::cerr << "portod not responding. sending sigkill" << std::endl;
    if (kill(pid, SIGKILL) && errno != ESRCH) {
        std::cerr << "cannot kill portod: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }
    error = MasterPidFile.Remove();
    if (error)
        std::cerr << "cannot remove pidfile: " << error << std::endl;
    error = PortodPidFile.Remove();
    if (error)
        std::cerr << "cannot remove pidfile: " << error << std::endl;
    return EXIT_SUCCESS;
}

static int ReexecPortod() {
    if (MasterPidFile.Load() || PortodPidFile.Load()) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (kill(MasterPidFile.Pid, SIGHUP)) {
        std::cerr << "cannot send signal" << std::endl;
        return EXIT_FAILURE;
    }

    uint64_t deadline = GetCurrentTimeMs() + config().daemon().portod_start_timeout() * 1000;
    do {
        if (!PortodPidFile.Running() && CheckPortoAlive())
            return EXIT_SUCCESS;
    } while (!WaitDeadline(deadline));

    std::cerr << "timeout exceeded" << std::endl;
    return EXIT_FAILURE;
}

int UpgradePortod() {
    TPath symlink(PORTO_BINARY_PATH), procexe("/proc/self/exe"), update, backup;
    uint64_t deadline;
    TError error;

    error = procexe.ReadLink(update);
    if (error) {
        std::cerr << "cannot read /proc/self/exe" << error << std::endl;
        return EXIT_FAILURE;
    }

    error = MasterPidFile.Load();
    if (error) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (!CheckPortoAlive()) {
        std::cerr << "portod running but not responding" << std::endl;
        return EXIT_FAILURE;
    }

    error = PortodPidFile.Load();
    if (error) {
        std::cerr << "cannot find portod: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = symlink.ReadLink(backup);
    if (error) {
        if (error.GetErrno() == ENOENT) {
            if (update != "/usr/sbin/portod") {
                std::cerr << "old portod can upgrade only to /usr/sbin/portod" << std::endl;
                return EXIT_FAILURE;
            }
        } else {
            std::cerr << "cannot read symlink " << symlink << ": " << error << std::endl;
            return EXIT_FAILURE;
        }
    }

    if (backup != update) {
        error = symlink.Unlink();
        if (error && error.GetErrno() != ENOENT) {
            std::cerr << "cannot remove old symlink: " << error << std::endl;
            return EXIT_FAILURE;
        }

        error = symlink.Symlink(update);
        if (error) {
            std::cerr << "cannot replace portod symlink: " << error << std::endl;
            goto undo;
        }
    }

    if (kill(MasterPidFile.Pid, SIGHUP)) {
        std::cerr << "online upgrade failed: " << strerror(errno) << std::endl;
        goto undo;
    }

    deadline = GetCurrentTimeMs() + config().daemon().portod_start_timeout() * 1000;
    do {
        if (!MasterPidFile.Running() || !PortodPidFile.Running())
            break;
    } while (!WaitDeadline(deadline));

    error = MasterPidFile.Load();
    if (error) {
        std::cerr << "online upgrade failed: " << error << std::endl;
        goto undo;
    }

    do {
        if (CheckPortoAlive()) {
            PrintVersion();
            return EXIT_SUCCESS;
        }
    } while (!WaitDeadline(deadline));

    std::cerr << "timeout exceeded" << std::endl;

undo:
    error = symlink.Unlink();
    if (error)
        std::cerr << "cannot remove symlink: " << error << std::endl;
    error = symlink.Symlink(backup);
    if (error)
        std::cerr << "cannot restore symlink: " << error << std::endl;
    return EXIT_FAILURE;
}

static void Usage() {
    std::cout
        << std::endl
        << "Usage: portod [options...] <command> [argments...]" << std::endl
        << std::endl
        << "Option: " << std::endl
        << "  -h | --help     print this message" << std::endl
        << "  -v | --version  print version and revision" << std::endl
        << "  --stdlog        print log into stdout" << std::endl
        << "  --norespawn     exit after failure" << std::endl
        << "  --verbose       verbose logging" << std::endl
        << "  --debug         debug logging" << std::endl
        << "  --discard       discard state after start" << std::endl
        << std::endl
        << "Commands: " << std::endl
        << "  status          check current portod status" << std::endl
        << "  daemon          start portod, this is default" << std::endl
        << "  start           daemonize and start portod" << std::endl
        << "  stop            stop running portod" << std::endl
        << "  restart         stop followed by start" << std::endl
        << "  reload          reexec portod" << std::endl
        << "  upgrade         upgrade running portod" << std::endl
        << "  dump            print internal key-value state" << std::endl
        << "  core            receive and forward core dump" << std::endl
        << "  help            print this message" << std::endl
        << "  version         print version and revision" << std::endl
        << std::endl;
}

int main(int argc, char **argv) {
    int opt = 0;

    while (++opt < argc && argv[opt][0] == '-') {
        std::string arg(argv[opt]);

        if (arg == "-v" || arg == "--version") {
            PrintVersion();
            return EXIT_SUCCESS;
        }

        if (arg == "-h" || arg == "--help") {
            Usage();
            return EXIT_SUCCESS;
        }

        if (arg == "--stdlog")
            StdLog = true;
        else if (arg == "--verbose")
            Verbose = true;
        else if (arg == "--debug")
            Verbose = Debug = true;
        else if (arg == "--norespawn")
            respawnPortod = false;
        else if (arg == "--discard")
            discardState = true;
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            Usage();
            return EXIT_FAILURE;
        }
    }

    std::string cmd(argv[opt] ?: "");

    if (cmd == "status") {
        if (!MasterPidFile.Path.Exists()) {
            std::cout << "stopped" << std::endl;
            return EXIT_FAILURE;
        } else if (CheckPortoAlive()) {
            std::cout << "running" << std::endl;
            return EXIT_SUCCESS;
        } else {
            std::cout << "unknown" << std::endl;
            return EXIT_FAILURE;
        }
    }

    if (cmd == "help") {
        Usage();
        return EXIT_SUCCESS;
    }

    if (cmd == "version") {
        PrintVersion();
        return EXIT_SUCCESS;
    }

    config.Load();

    if (cmd == "" || cmd == "daemon")
        return PortodMain();

    if (cmd == "start")
        return StartPortod();

    if (cmd == "stop")
        return StopPortod();

    if (cmd == "restart") {
        StopPortod();
        return StartPortod();
    }

    if (cmd == "reload")
        return ReexecPortod();

    if (cmd == "upgrade")
        return UpgradePortod();

    if (cmd == "dump") {
        KvDump();
        return EXIT_SUCCESS;
    }

    if (cmd == "core") {
        TCore core;
        TError error = core.Handle(TTuple(argv + opt + 1, argv + argc));
        if (error)
            return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }

    std::cerr << "Unknown command: " << cmd << std::endl;
    Usage();
    return EXIT_FAILURE;
}
