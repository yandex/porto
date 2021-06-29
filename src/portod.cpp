#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <iostream>

#include "version.hpp"
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

std::unordered_set<pid_t> PortoTids;
std::mutex TidsMutex;

pid_t MasterPid;
pid_t PortodPid;
static int PortodStatus;

static ino_t SocketIno = 0;

static bool RespawnPortod = true;
static bool DiscardState = false;

static int CmdTimeout = -1;

static std::map<pid_t, int> Zombies;

bool PortodFrozen = false;
bool ShutdownPortod = false;
static uint64_t ShutdownStart = 0;
static uint64_t ShutdownDeadline = 0;
std::atomic_bool NeedStopHelpers(false);

bool SupportCgroupNs = false;
bool EnableOsModeCgroupNs = false;
bool EnableRwCgroupFs = false;
bool EnableDockerMode = false;
uint32_t RequestHandlingDelayMs = 0;

static bool RunningInContainer() {
    if (getpid() == 1)
        return getenv("container") != nullptr;
    std::string env;
    if (TPath("/proc/1/environ").ReadAll(env))
        return false;
    return StringStartsWith(env, "container=") ||
        env.find(std::string("\0container=", 11)) != std::string::npos;
}

static bool CheckPortoAlive() {
    Porto::Connection conn;
    if (conn.SetTimeout(1) != EError::Success)
        return false;
    std::string ver, rev;
    return !conn.GetVersion(ver, rev);
}

static bool SanityCheck() {
    if (getuid() != 0) {
        std::cerr << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    if (!MasterPidFile.Read() && MasterPidFile.Pid != getpid() && CheckPortoAlive()) {
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

static TError CreatePortoSocket() {
    TPath path(PORTO_SOCKET_PATH);
    struct stat fd_stat, sk_stat;
    struct sockaddr_un addr;
    TError error;
    TFile sock;

    if (dup2(PORTO_SK_FD, PORTO_SK_FD) == PORTO_SK_FD) {
        sock.SetFd = PORTO_SK_FD;
        if (!sock.Stat(fd_stat) && S_ISSOCK(fd_stat.st_mode) &&
                !path.StatStrict(sk_stat) && S_ISSOCK(sk_stat.st_mode)) {
            time_t now = time(nullptr);
            L_SYS("Reuse porto socket: inode {} : {} "
                  "age {} : {}",
                  fd_stat.st_ino, sk_stat.st_ino,
                  now - fd_stat.st_ctime, now - sk_stat.st_ctime);
            SocketIno = sk_stat.st_ino;
        } else {
            L_WRN("Unlinked porto socket. Recreating...");
            sock.SetFd = -1;
        }
    }

    if (!sock) {
        sock.SetFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (sock.Fd < 0)
            return TError::System("socket()");

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        (void)path.Unlink();

        if (bind(sock.Fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
            return TError::System("bind()");

        error = path.StatStrict(sk_stat);
        if (error)
            return error;
        SocketIno = sk_stat.st_ino;
    }

    if (fchmod(sock.Fd, PORTO_SOCKET_MODE) < 0)
        return TError::System("fchmod()");

    error = path.Chown(RootUser, PortoGroup);
    if (error)
        return error;

    error = path.Chmod(PORTO_SOCKET_MODE);
    if (error)
        return error;

    if (listen(sock.Fd, config().daemon().max_clients()) < 0)
        return TError::System("listen()");

    if (sock.Fd == PORTO_SK_FD)
        sock.SetFd = -1;
    else if (dup2(sock.Fd, PORTO_SK_FD) != PORTO_SK_FD)
        return TError::System("dup2()");

    return OK;
}

void CheckPortoSocket() {
    struct stat fd_stat, sk_stat;
    TError error;

    if (fstat(PORTO_SK_FD, &fd_stat))
        error = TError::System("socket fd stat");
    else if (stat(PORTO_SOCKET_PATH, &sk_stat))
        error = TError::System("socket path stat");
    else if (!S_ISSOCK(fd_stat.st_mode) || !S_ISSOCK(sk_stat.st_mode))
        error = TError::System("not a socket");
    else if (sk_stat.st_ino != SocketIno)
        error = TError::System("different inode");
    else
        return;

    L_WRN("Porto socket: {}", error);
    kill(MasterPid, SIGHUP);
}

void AckExitStatus(int pid) {
    if (!pid)
        return;

    L_DBG("Acknowledge exit status for {}", pid);
    int ret = write(REAP_ACK_FD, &pid, sizeof(pid));
    if (ret != sizeof(pid)) {
        L_ERR("Can't acknowledge exit status for {}: {}", pid, strerror(errno));
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

        if (client->Processing || client->Sending)
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

    L_SYS("Kick client {} idle={} ms", victim->Id, idle);
    Clients.erase(victim->Fd);
    victim->CloseConnection();
    return OK;
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
            return OK; /* client already gone */
        return TError::System("accept4()");
    }

    Statistics->ClientsConnected++;

    auto client = std::make_shared<TClient>(clientFd);
    error = client->IdentifyClient();
    if (error)
        return error;

    unsigned max_clients = config().daemon().max_clients_in_container();
    if (client->IsSuperUser())
        max_clients += NR_SUPERUSER_CLIENTS;

    if (client->ClientContainer->ClientsCount > (int)max_clients) {
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

    return OK;
}

static void StartShutdown() {
    ShutdownPortod = true;
    ShutdownStart = GetCurrentTimeMs();
    ShutdownDeadline = ShutdownStart + config().daemon().portod_shutdown_timeout() * 1000;

    /* Stop accepting new clients */
    EpollLoop->RemoveSource(PORTO_SK_FD);

    /* Kick idle clients */
    for (auto it = Clients.begin(); it != Clients.end(); ) {
        auto client = it->second;

        if (client->IsBlockShutdown()) {
            L_SYS("Client blocks shutdown: {}", client->Id);
            ++it;
        } else {
            client->CloseConnection();
            it = Clients.erase(it);
        }
    }
}

static void PortodServer() {
    TError error;

    auto AcceptSource = std::make_shared<TEpollSource>(PORTO_SK_FD);
    error = EpollLoop->AddSource(AcceptSource);
    if (error) {
        L_ERR("Can't add RPC server fd to epoll: {}", error);
        return;
    }

    auto MasterSource = std::make_shared<TEpollSource>(REAP_EVT_FD);
    error = EpollLoop->AddSource(MasterSource);
    if (error) {
        L_ERR("Can't add master fd to epoll: {}", error);
        return;
    }

    /* Don't disturb threads. Deliver signals via signalfd. */
    int sigFd = SignalFd();

    auto sigSource = std::make_shared<TEpollSource>(sigFd);
    error = EpollLoop->AddSource(sigSource);
    if (error) {
        L_ERR("Can't add sigSource to epoll: {}", error);
        return;
    }

    StartStatFsLoop();
    StartRpcQueue();
    EventQueue->Start();

    if (config().daemon().log_rotate_ms()) {
        TEvent ev(EEventType::RotateLogs);
        EventQueue->Add(config().daemon().log_rotate_ms(), ev);
    }

    std::vector<struct epoll_event> events;

    while (true) {
        error = EpollLoop->GetEvents(events, 1000);
        if (error) {
            L_ERR("epoll error {}", error);
            goto exit;
        }

        if (RecvExitEvents(REAP_EVT_FD))
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
                        DiscardState = true;
                        RespawnPortod = false;
                        L_SYS("Shutdown...");
                        StartShutdown();
                        break;
                    case SIGTERM:
                        RespawnPortod = false;
                        L_SYS("Shutdown...");
                        StartShutdown();
                        break;
                    case SIGHUP:
                        L_SYS("Updating...");
                        StartShutdown();
                        break;
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
                if (error && Verbose)
                    L_SYS("Cannot accept connection: {}", error);
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
                error = client->Event(ev.events);
                if (error) {
                    Clients.erase(source->Fd);
                    client->CloseConnection();
                }
            } else {
                L_WRN("Unknown event {}", source->Fd);
                EpollLoop->RemoveSource(source->Fd);
            }
        }

        if (ShutdownPortod) {
            if (Clients.empty()) {
                L_SYS("All clients are gone");
                break;
            }
            if (int64_t(ShutdownDeadline - GetCurrentTimeMs()) < 0) {
                L_SYS("Shutdown timeout exceeded");
                TContainer::DumpLocks();
                break;
            }
        }
    }

exit:

    for (auto c : Clients)
        c.second->CloseConnection(true);
    Clients.clear();
    NeedStopHelpers = true;

    L_SYS("Stop threads...");
    EventQueue->Stop();
    StopRpcQueue();
    StopStatFsLoop();
}

static TError TuneLimits() {
    TUlimit ulimit;
    TError error;

    /*
     * two FDs for each container: OOM event and netlink
     * ten for each thread
     * one for each client
     * plus some extra
     */
    int maxFd = config().container().max_total() * 2 +
                NR_SUPERUSER_CONTAINERS * 2 +
                (config().daemon().ro_threads() +
                 config().daemon().rw_threads() +
                 config().daemon().io_threads() +
                 config().daemon().vl_threads()) * 10 +
                config().daemon().max_clients() +
                NR_SUPERUSER_CLIENTS +
                1000;

    L_SYS("Estimated portod file descriptor limit: {}", maxFd);

    ulimit.Set(RLIMIT_NOFILE, maxFd, maxFd);

    /*
     * Old make set unlimited stack
     */
    ulimit.Set(RLIMIT_STACK, 8 << 20, RLIM_INFINITY);

    error = ulimit.Apply();
    if (error)
        return error;

    error = ulimit.Load();
    if (error)
        return error;

    for (auto &res: ulimit.Resources)
        L_SYS("Ulimit {}", res.Format());

    return OK;
}

static TError CreateRootContainer() {
    TError error;

    error = TContainer::Create(ROOT_CONTAINER, RootContainer);
    if (error)
        return error;

    PORTO_ASSERT(RootContainer->Id == ROOT_CONTAINER_ID);
    PORTO_ASSERT(RootContainer->IsRoot());

    RootContainer->Isolate = false;

    error = RootContainer->Ulimit.Parse(config().container().default_ulimit());
    if (error)
        return error;

    uint64_t pids_max, threads_max;
    std::string str;
    if (!GetSysctl("kernel.pid_max", str) && !StringToUint64(str, pids_max) &&
            !GetSysctl("kernel.threads-max", str) && !StringToUint64(str, threads_max)) {
        uint64_t lim = std::min(pids_max, threads_max) / 2;
        L_SYS("Default nproc ulimit: {}", lim);
        RootContainer->Ulimit.Set(TUlimit::GetType("nproc"), lim, lim, false);
    }

    error = RootContainer->Devices.InitDefault();
    if (error)
        return error;

    error = SystemClient.LockContainer(RootContainer);
    if (error)
        return error;

    error = RootContainer->Start();
    if (error)
        return error;

    error = ContainerIdMap.GetAt(DEFAULT_CONTAINER_ID);
    if (error)
        return error;

    error = ContainerIdMap.GetAt(LEGACY_CONTAINER_ID);
    if (error)
        return error;

    SystemClient.ReleaseContainer();

    TNetwork::SyncResolvConf();

    return OK;
}

static void RestoreContainers() {
    TIdMap ids(4, CONTAINER_ID_MAX - 4);
    std::list<TKeyValue> nodes;

    TError error = TKeyValue::ListAll(ContainersKV, nodes);
    if (error)
        FatalError("Cannot list container kv", error);

    for (auto node = nodes.begin(); node != nodes.end(); ) {
        error = node->Load();
        if (!error) {
            if (!node->Has(P_RAW_ID))
                error = TError("id not found");
            if (!node->Has(P_RAW_NAME))
                error = TError("name not found");
            if (!error && (StringToInt(node->Get(P_RAW_ID), node->Id) ||
                           (node->Id > 3 && ids.GetAt(node->Id))))
                node->Id = 0;
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

    for (auto &node : nodes) {
        if (node.Name[0] != '/' && !node.Id) {
            error = ids.Get(node.Id);
            if (!error) {
                L("Replace container {} id {}", node.Name, node.Id);
                TPath path = ContainersKV / std::to_string(node.Id);
                node.Path.Rename(path);
                node.Path = path;
                node.Set(P_RAW_ID, std::to_string(node.Id));
                node.Save();
            }
        }
    }

    nodes.sort();

    for (auto &node : nodes) {
        if (node.Name[0] == '/')
            continue;

        std::shared_ptr<TContainer> ct;
        error = TContainer::Restore(node, ct);
        if (error) {
            L_ERR("Cannot restore {}: {}", node.Name, error);
            Statistics->ContainerLost++;
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
                    (hy->Controllers & (CGROUP_FREEZER | CGROUP_MEMORY | CGROUP_CPUACCT | CGROUP2 | CGROUP_PERF)))
                continue;

            if (cg->Name == PORTO_HELPERS_CGROUP &&
                    (hy->Controllers & CGROUP_MEMORY))
                continue;

            if (cg->Name == PORTO_CGROUP_PREFIX &&
                    (hy->Controllers & (CGROUP_FREEZER | CGROUP2)))
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

static void CleanupWorkdir() {
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
            L_VERBOSE("Cannot remove workdir {}: {}", path, error);
            error = path.RemoveAll();
            if (error)
                L_WRN("Cannot remove workdir {}: {}", path, error);
        }
    }
}

static void DestroyContainers(bool weak) {
    std::list<std::shared_ptr<TVolume>> unlinked;

    SystemClient.LockContainer(RootContainer);

    /* leaves first */
    for (auto &ct: RootContainer->Subtree()) {
        if (ct->IsRoot() || (weak && !ct->IsWeak))
            continue;

        TError error = ct->Destroy(unlinked);
        if (error)
            L_ERR("Cannot destroy container {}: {}", ct->Name, error);
    }

    SystemClient.ReleaseContainer();

    TVolume::DestroyUnlinked(unlinked);
}

static int Portod() {
    TError error;

    SetDieOnParentExit(SIGKILL);

    ResetStatistics();

    Statistics->PortoStarted = GetCurrentTimeMs();

    SetProcessName(PORTOD_NAME);

    OpenLog(PORTO_LOG);
    if (!LogFile)
        return EXIT_FAILURE;

    PortodPid = getpid();
    error = PortodPidFile.Save(PortodPid);
    if (error)
        FatalError("Cannot save pid", error);

    ReadConfigs();

    SupportCgroupNs = CompareVersions(config().linux_version(), "4.6") >= 0;
    if (SupportCgroupNs) {
        EnableRwCgroupFs = config().container().enable_rw_cgroupfs();
        EnableOsModeCgroupNs = EnableRwCgroupFs || config().container().use_os_mode_cgroupns();
        EnableDockerMode = config().container().enable_docker_mode() && EnableOsModeCgroupNs;
    }
    RequestHandlingDelayMs = config().daemon().request_handling_delay_ms();

    InitPortoGroups();
    InitCapabilities();
    InitIpcSysctl();
    InitProcBaseDirs();
    TNetwork::InitializeConfig();

    L_SYS("Portod config:\n{}", config().DebugString());

    SetPtraceProtection(config().daemon().ptrace_protection());

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

    TPath root("/");
    error = root.Chdir();
    if (error)
        FatalError("Cannot chdir to /", error);

    // We want propagate mounts into containers
    error = root.Remount(MS_SHARED | MS_REC);
    if (error)
        FatalError("Cannot remount / recursively as shared", error);

    TPath tracefs = "/sys/kernel/tracing";
    if (config().container().enable_tracefs() && tracefs.Exists()) {
        error = tracefs.Mount("none", "tracefs", MS_NOEXEC | MS_NOSUID | MS_NODEV, {"mode=755"});
        if (error && error.Errno != EBUSY)
            L_SYS("Cannot mount tracefs: {}", error);
    }

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

    L_SYS("Restore containers...");
    TCgroup::StartRestore();
    RestoreContainers();
    TCgroup::FinishRestore();

    L_SYS("Restore statistics...");
    TContainer::SyncPropertiesAll();

    L_SYS("Restore volumes...");
    TVolume::RestoreAll();

    DestroyContainers(true);

    if (DiscardState) {
        DiscardState = false;

        L_SYS("Destroy containers...");
        DestroyContainers(false);

        L_SYS("Destroy volumes...");
        TVolume::DestroyAll();
    }

    SystemClient.FinishRequest();

    L_SYS("Cleanup cgroup...");
    CleanupCgroups();

    L_SYS("Cleanup workdir...");
    CleanupWorkdir();

    L_SYS("Restore complete. time={} ms", GetCurrentTimeMs() - Statistics->PortoStarted);

    PortodServer();

    if (DiscardState) {
        DiscardState = false;

        SystemClient.StartRequest();

        L_SYS("Stop containers...");

        SystemClient.LockContainer(RootContainer);
        error = RootContainer->Stop(0);
        SystemClient.ReleaseContainer();
        if (error)
            L_ERR("Failed to stop root container and its children {}", error);

        L_SYS("Destroy containers...");
        DestroyContainers(false);

        L_SYS("Destroy volumes...");
        TVolume::DestroyAll();

        std::list<std::shared_ptr<TVolume>> unlinked;

        SystemClient.LockContainer(RootContainer);
        error = RootContainer->Destroy(unlinked);
        SystemClient.ReleaseContainer();
        if (error)
            L_ERR("Cannot destroy root container{}", error);

        TVolume::DestroyUnlinked(unlinked);

        SystemClient.FinishRequest();

        RootContainer = nullptr;

        error = ContainersKV.UmountAll();
        if (error)
            L_ERR("Can't destroy key-value storage: {}", error);

        error = VolumesKV.UmountAll();
        if (error)
            L_ERR("Can't destroy volume key-value storage: {}", error);
    }

    PortodPidFile.Remove();

    // move master to root, otherwise older version will kill itself
    FreezerSubsystem.RootCgroup().Attach(MasterPid);

    L_SYS("Shutdown complete. time={} ms", GetCurrentTimeMs() - ShutdownStart);

    return EXIT_SUCCESS;
}

static void UpdateQueueSize() {
    Statistics->QueuedStatuses = Zombies.size();
}

static void ReportZombies(int fd) {
    while (true) {
        siginfo_t info;

        info.si_pid = 0;
        if (waitid(P_PID, PortodPid, &info, WNOHANG | WNOWAIT | WEXITED) || !info.si_pid)
            if (waitid(P_ALL, -1, &info, WNOHANG | WNOWAIT | WEXITED) || !info.si_pid)
                break;

        pid_t pid = info.si_pid;
        int status = 0;
        if (info.si_code == CLD_KILLED) {
            status = info.si_status;
        } else if (info.si_code == CLD_DUMPED) {
            status = info.si_status | (1 << 7);
        } else { // CLD_EXITED
            status = info.si_status << 8;
        }

        if (pid == PortodPid) {
            (void)waitpid(pid, NULL, 0);
            PortodPid = 0;
            PortodStatus = status;
            break;
        }

        if (Zombies.count(pid))
            break;

        L_VERBOSE("Report zombie pid={} status={}", pid, status);
        int report[2] = {pid, status};
        if (write(fd, report, sizeof(report)) != sizeof(report)) {
            L_WRN("Cannot report zombie: {}", TError::System("write"));
            break;
        }

        Zombies[pid] = status;
        UpdateQueueSize();
    }
}

static int ReapZombies(int fd) {
    int pid;
    int nr = 0;

    while (read(fd, &pid, sizeof(pid)) == sizeof(pid)) {
        if (pid <= 0)
            continue;

        if (Zombies.find(pid) == Zombies.end()) {
            L_WRN("Got ack for unknown zombie pid={}", pid);
        } else {
            L_VERBOSE("Reap zombie pid={}", pid);
            (void)waitpid(pid, NULL, 0);
            Zombies.erase(pid);
            UpdateQueueSize();
        }

        nr++;
    }

    return nr;
}

static int UpgradeMaster() {
    L_SYS("Updating...");

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

static void SpawnPortod(std::shared_ptr<TEpollLoop> loop) {
    int evtfd[2];
    int ackfd[2];
    TError error;

    if (pipe2(evtfd, O_NONBLOCK | O_CLOEXEC) < 0) {
        L_ERR("pipe(): {}", strerror(errno));
        return;
    }

    if (pipe2(ackfd, O_NONBLOCK | O_CLOEXEC) < 0) {
        L_ERR("pipe(): {}", strerror(errno));
        return;
    }

    auto AckSource = std::make_shared<TEpollSource>(ackfd[0]);

    int sigFd = SignalFd();

    auto sigSource = std::make_shared<TEpollSource>(sigFd);

    /* Forget all zombies to report them again */
    Zombies.clear();
    UpdateQueueSize();

    PortodPid = fork();
    if (PortodPid < 0) {
        L_ERR("fork(): {}", strerror(errno));
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

    L_SYS("Start portod {}", PortodPid);
    Statistics->PortoStarts++;

    error = loop->AddSource(AckSource);
    if (error) {
        L_ERR("Can't add ackfd[0] to epoll: {}", error);
        goto exit;
    }

    error = loop->AddSource(sigSource);
    if (error) {
        L_ERR("Can't add sigSource to epoll: {}", error);
        goto exit;
    }

    while (PortodPid) {
        std::vector<struct epoll_event> events;

        error = loop->GetEvents(events, -1);
        if (error) {
            L_ERR("master: epoll error {}", error);
            goto exit;
        }

        struct signalfd_siginfo sigInfo;

        while (read(sigFd, &sigInfo, sizeof sigInfo) == sizeof sigInfo) {
            int signo = sigInfo.ssi_signo;

            switch (signo) {
            case SIGINT:
            case SIGTERM: {
                L_SYS("Forward signal {} to portod", signo);
                if (kill(PortodPid, signo) < 0)
                    L_ERR("Cannot kill portod: {}", TError::System("kill"));

                L_SYS("Waiting for portod shutdown...");
                uint64_t deadline = GetCurrentTimeMs() +
                                    config().daemon().portod_stop_timeout() * 1000;
                do {
                    if (waitpid(PortodPid, &PortodStatus, WNOHANG) == PortodPid) {
                        PortodPid = 0;
                        break;
                    }
                } while (!WaitDeadline(deadline));

                RespawnPortod = false;
                goto exit;
            }
            case SIGUSR1: {
                OpenLog(PORTO_LOG);
                if (kill(PortodPid, signo) < 0)
                    L_ERR("Cannot kill portod: {}", TError::System("kill"));
                break;
            }
            case SIGUSR2:
                DumpMallocInfo();
                break;
            case SIGHUP:
                UpgradeMaster();
                break;
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
                if (!ReapZombies(ackfd[0])) {
                    goto exit;
                }
            } else {
                L_WRN("Unknown event {}", source->Fd);
                loop->RemoveSource(source->Fd);
            }
        }

        ReportZombies(evtfd[1]);
    }

exit:
    if (PortodPid) {
        L_SYS("Kill portod");
        if (kill(PortodPid, SIGKILL) < 0)
            L_ERR("Cannot kill portod: {}", TError::System("kill"));
        (void)waitpid(PortodPid, &PortodStatus, 0);
        PortodPid = 0;
    }

    L_SYS("Portod {}", FormatExitStatus(PortodStatus));

    loop->RemoveSource(sigFd);
    close(sigFd);

    loop->RemoveSource(AckSource->Fd);

    close(evtfd[1]);
    close(ackfd[0]);
}

static int PortodMaster() {
    TError error;
    int ret;

    if (SanityCheck())
        return EXIT_FAILURE;

    SetProcessName(PORTOD_MASTER_NAME);

    (void)close(STDIN_FILENO);
    int null = open("/dev/null", O_RDWR);
    PORTO_ASSERT(null == STDIN_FILENO);

    if (!StdLog || fcntl(STDOUT_FILENO, F_GETFD) < 0) {
        ret = dup2(null, STDOUT_FILENO);
        PORTO_ASSERT(ret == STDOUT_FILENO);
    }

    if (!StdLog || fcntl(STDERR_FILENO, F_GETFD) < 0) {
        ret = dup2(null, STDERR_FILENO);
        PORTO_ASSERT(ret == STDERR_FILENO);
    }

    OpenLog(PORTO_LOG);
    if (!LogFile)
        return EXIT_FAILURE;

    InitStatistics();

    Statistics->MasterStarted = GetCurrentTimeMs();

    ret = chdir("/");
    PORTO_ASSERT(!ret);

    CatchFatalSignals();

    MasterPid = getpid();
    error = MasterPidFile.Save(MasterPid);
    if (error)
        FatalError("Cannot save pid", error);

    ReadConfigs();
    error = ValidateConfig();
    if (error)
        FatalError("Invalid config", error);

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

    do {
        uint64_t started = GetCurrentTimeMs();
        uint64_t next = started + config().container().respawn_delay_ms();

        SpawnPortod(ELoop);

        if (next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);

        PreviousVersion = PORTO_VERSION;
    } while (RespawnPortod);

    error = TCore::Unregister();
    if (error)
        L_ERR("Cannot revert core pattern: {}", error);

    error = TPath(PORTO_SOCKET_PATH).Unlink();
    if (error)
        L_ERR("Cannot unlink socket file: {}", error);

    PortodPidFile.Remove();
    MasterPidFile.Remove();
    pathBin.Unlink();
    pathVer.Unlink();
    TPath(PORTO_CONTAINERS_KV).Rmdir();
    TPath(PORTO_VOLUMES_KV).Rmdir();
    TPath("/run/porto").Rmdir();
    TPath(PORTOD_STAT_FILE).Unlink();

    L_SYS("Shutdown complete.");

    return EXIT_SUCCESS;
}

static void KvDump() {
    TKeyValue::DumpAll(PORTO_CONTAINERS_KV);
    TKeyValue::DumpAll(PORTO_VOLUMES_KV);
}

static void PrintVersion() {
    TPath thisBin, currBin;

    TPath("/proc/self/exe").ReadLink(thisBin);
    if (MasterPidFile.Read() || TPath("/proc/" +
                std::to_string(MasterPidFile.Pid) +"/exe").ReadLink(currBin))
        TPath(PORTO_BINARY_PATH).ReadLink(currBin);

    std::cout << "version: " << PORTO_VERSION << " " << PORTO_REVISION << " " << thisBin << std::endl;

    Porto::Connection conn;
    std::string ver, rev;
    if (!conn.GetVersion(ver, rev))
        std::cout << "running: " <<  ver + " " + rev << " " << currBin << std::endl;
}

static int StartPortod() {
    if (SanityCheck())
        return EXIT_FAILURE;

    pid_t pid = fork();
    if (pid < 0)
        return EXIT_FAILURE;

    if (!pid)
        return PortodMaster();

    uint64_t timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_start_timeout();
    uint64_t deadline = GetCurrentTimeMs() + timeout * 1000;
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

static int KillPortod() {
    TError error;

    if (MasterPidFile.Read()) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_SUCCESS;
    }

    error = MasterPidFile.Remove();
    if (error)
        std::cerr << "cannot remove pidfile: " << error << std::endl;

    error = PortodPidFile.Remove();
    if (error)
        std::cerr << "cannot remove pidfile: " << error << std::endl;

    if (kill(MasterPidFile.Pid, SIGKILL) && errno != ESRCH) {
        std::cerr << "cannot kill portod: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int StopPortod() {
    TError error;

    if (MasterPidFile.Read()) {
        std::cerr << "portod already stopped" << std::endl;
        return EXIT_SUCCESS;
    }

    uint64_t timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_stop_timeout();

    pid_t pid = MasterPidFile.Pid;
    if (timeout > 0 && CheckPortoAlive()) {
        if (!kill(pid, SIGINT)) {
            uint64_t deadline = GetCurrentTimeMs() + timeout * 1000;
            do {
                if (MasterPidFile.Read() || MasterPidFile.Pid != pid)
                    return EXIT_SUCCESS;
            } while (!WaitDeadline(deadline));
        } else if (errno != ESRCH) {
            std::cerr << "cannot stop portod: " << strerror(errno) << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::cerr << "portod not responding. sending sigkill" << std::endl;
    return KillPortod();
}

static int ReexecPortod() {
    if (MasterPidFile.Read() || PortodPidFile.Read()) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (kill(MasterPidFile.Pid, SIGHUP)) {
        std::cerr << "cannot send signal" << std::endl;
        return EXIT_FAILURE;
    }

    uint64_t timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_start_timeout();
    uint64_t deadline = GetCurrentTimeMs() + timeout * 1000;
    do {
        if (!MasterPidFile.Running())
            return EXIT_FAILURE;
        if (CheckPortoAlive())
            return EXIT_SUCCESS;
    } while (!WaitDeadline(deadline));

    std::cerr << "timeout exceeded" << std::endl;
    return EXIT_FAILURE;
}

int UpgradePortod() {
    TPath symlink(PORTO_BINARY_PATH), procexe("/proc/self/exe"), update, backup;
    uint64_t timeout, deadline;
    TError error;

    error = procexe.ReadLink(update);
    if (error) {
        std::cerr << "cannot read /proc/self/exe" << error << std::endl;
        return EXIT_FAILURE;
    }

    error = MasterPidFile.Read();
    if (error) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (!CheckPortoAlive()) {
        std::cerr << "portod running but not responding" << std::endl;
        return EXIT_FAILURE;
    }

    error = PortodPidFile.Read();
    if (error) {
        std::cerr << "cannot find portod: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = symlink.ReadLink(backup);
    if (error) {
        if (error.Errno == ENOENT) {
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
        if (error && error.Errno != ENOENT) {
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

    timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_start_timeout();
    deadline = GetCurrentTimeMs() + timeout * 1000;
    do {
        if (!MasterPidFile.Running() || !PortodPidFile.Running())
            break;
    } while (!WaitDeadline(deadline));

    error = MasterPidFile.Read();
    if (error) {
        std::cerr << "online upgrade failed: " << error << std::endl;
        goto undo;
    }

    do {
        if (!MasterPidFile.Running())
            return EXIT_FAILURE;
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

int ReopenLog() {
    TError error;

    error = MasterPidFile.Read();
    if (error) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (kill(MasterPidFile.Pid, SIGUSR1) && errno != ESRCH) {
        std::cerr << "cannot send signal to portod: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

void ReopenMasterLog() {
    if (MasterPid)
        kill(MasterPid, SIGUSR1);
}

static int GetSystemProperties() {
    Porto::Connection conn;
    std::string rsp;
    int ret = conn.Call("GetSystem {}", rsp);
    if (ret) {
        std::cerr << conn.GetLastError() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << rsp << std::endl;
    return EXIT_SUCCESS;
}

static int SetSystemProperties(TTuple arg) {
    Porto::Connection conn;
    std::string rsp;
    if (arg.size() != 2)
        return EXIT_FAILURE;
    int ret = conn.Call(fmt::format("SetSystem {{ {}: {} }}", arg[0], arg[1]), rsp);
    if (ret) {
        std::cerr << conn.GetLastError() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int ClearStatistics(TTuple arg) {
    Porto::Connection conn;
    std::string rsp;
    std::string req;
    if (arg.size() == 1)
        req = fmt::format("ClearStatistics {{ stat: \"{}\" }}", arg[0]);
    else
        req = "ClearStatistics {}";

    int ret = conn.Call(req, rsp);
    if (ret) {
        std::cerr << conn.GetLastError() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static void Usage() {
    std::cout
        << std::endl
        << "Usage: portod [options...] <command> [argments...]" << std::endl
        << std::endl
        << "Option: " << std::endl
        << "  -h | --help      print this message" << std::endl
        << "  -v | --version   print version and revision" << std::endl
        << "  --stdlog         print log into stdout" << std::endl
        << "  --norespawn      exit after failure" << std::endl
        << "  --verbose        verbose logging" << std::endl
        << "  --debug          debug logging" << std::endl
        << "  --discard        discard state after start" << std::endl
        << std::endl
        << "Commands: " << std::endl
        << "  status           check current portod status" << std::endl
        << "  daemon           start portod, this is default" << std::endl
        << "  start            daemonize and start portod" << std::endl
        << "  stop             stop running portod" << std::endl
        << "  kill             kill running portod" << std::endl
        << "  restart          stop followed by start" << std::endl
        << "  reload           reexec portod" << std::endl
        << "  reopenlog        reopen portod.log" << std::endl
        << "  upgrade          upgrade running portod" << std::endl
        << "  dump             print internal key-value state" << std::endl
        << "  get              print system properties" << std::endl
        << "  set <key> <val>  change system properties" << std::endl
        << "  clearstat [stat] reset statistics" << std::endl
        << "  freeze           freeze changes" << std::endl
        << "  unfreeze         unfreeze changes" << std::endl
        << "  core             receive and forward core dump" << std::endl
        << "  help             print this message" << std::endl
        << "  version          print version and revision" << std::endl
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
            RespawnPortod = false;
        else if (arg == "--discard")
            DiscardState = true;
        else if (arg == "--timeout") {
           if (StringToInt(argv[++opt], CmdTimeout))
               return EXIT_FAILURE;
        } else {
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

    ReadConfigs(true);

    if (cmd == "" || cmd == "daemon")
        return PortodMaster();

    if (cmd == "start")
        return StartPortod();

    if (cmd == "stop")
        return StopPortod();

    if (cmd == "kill")
        return KillPortod();

    if (cmd == "restart") {
        StopPortod();
        return StartPortod();
    }

    if (cmd == "reload")
        return ReexecPortod();

    if (cmd == "upgrade")
        return UpgradePortod();

    if (cmd == "reopenlog")
        return ReopenLog();

    if (cmd == "dump") {
        OpenLog();
        KvDump();
        return EXIT_SUCCESS;
    }

    if (cmd == "get")
        return GetSystemProperties();

    if (cmd == "set")
        return SetSystemProperties(TTuple(argv + opt + 1, argv + argc));

    if (cmd == "clearstat")
        return ClearStatistics(TTuple(argv + opt + 1, argv + argc));

    if (cmd == "freeze")
        return SetSystemProperties({"frozen", "true"});

    if (cmd == "unfreeze")
        return SetSystemProperties({"frozen", "false"});

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
