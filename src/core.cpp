#include "core.hpp"
#include "config.hpp"
#include "container.hpp"
#include "property.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
}

TError TCore::Register(const TPath &portod) {
    std::string limit, pattern;
    TError error;

    if (!config().core().enable())
        return OK;

    error = GetSysctl("kernel.core_pipe_limit", limit);
    if (error || limit == "0") {
        /* wait for herlper exit - keep pidns alive */
        error = SetSysctl("kernel.core_pipe_limit", "1024");
        if (error)
            return error;
    }

    pattern = "|" + portod.ToString() + " core %P %I %p %i %s %d %c " +
              StringReplaceAll(config().core().default_pattern(), " ", "__SPACE__") +
              " %u %g";
    return SetSysctl("kernel.core_pattern", pattern);
}

TError TCore::Unregister() {
    if (!config().core().enable())
        return OK;

    return SetSysctl("kernel.core_pattern", config().core().default_pattern());
}

TError TCore::Handle(const TTuple &args) {
    TError error;

    if (args.size() < 7)
        return TError("should be executed via sysctl kernel.core_pattern");

    SetProcessName("portod-core");
    CatchFatalSignals();
    OpenLog(PORTO_LOG);

    L_CORE("Args {}", MergeEscapeStrings(args, '\t'));

    Pid = std::stoi(args[0]);
    Tid = std::stoi(args[1]);
    Vpid = std::stoi(args[2]);
    Vtid = std::stoi(args[3]);
    Signal = std::stoi(args[4]);
    Dumpable = std::stoi(args[5]);
    Ulimit = std::stoull(args[6]);
    if (args.size() > 7)
        DefaultPattern = StringReplaceAll(args[7], "__SPACE__", " ");

    if (args.size() > 9) {
        OwnerUid = std::stoi(args[8]);
        OwnerGid = std::stoi(args[9]);
    }

    ProcessName = GetTaskName(Pid);
    ThreadName = GetTaskName(Tid);

    error = TPath("/proc/" + std::to_string(Tid) + "/exe").ReadLink(ExePath);
    if (!error) {
        ExeName = ExePath.BaseName();
        if (StringEndsWith(ExeName, " (deleted)"))
            ExeName = ExeName.substr(0, ExeName.size() - 10);
    } else {
        L("Cannot get exe file path: {}", error);
        ExeName = ProcessName;
    }

    error = Identify();

    for (auto name = Container; name != "/"; name = TContainer::ParentName(name))
        Conn.IncLabel(name, "CORE.total");

    /* protect host suid binaries */
    if (Dumpable != 1 && RootPath == "/")
        CoreCommand = "";

    if (!Ulimit || (Dumpable == 0 && CoreCommand == "")) {
        L_CORE("Ignore core from CT:{} {} {}:{} thread {}:{} signal {}, ulimit {} dumpable {}",
                Container, ExeName, Pid, ProcessName, Tid, ThreadName, Signal,
                Ulimit, Dumpable);
        return OK;
    }

    if (CoreCommand != "" &&
        (State == TContainer::StateName(EContainerState::Running) || State == TContainer::StateName(EContainerState::Meta))) {
        L_CORE("Forward core from CT:{} {} {}:{} thread {}:{} signal {} dumpable {}",
                Container, ExeName, Pid, ProcessName, Tid, ThreadName, Signal, Dumpable);

        error = Forward();
        if (error) {
            L("Cannot forward core from CT:{}: {}", Container, error);
            CoreCommand = "";
        }
    }

    if (CoreCommand == "" && DefaultPattern) {
        L_CORE("Save core from CT:{} {} {}:{} thread {}:{} signal {} dumpable {}",
                Container, ExeName, Pid, ProcessName, Tid, ThreadName, Signal, Dumpable);

        error = Save();
        if (error)
            L("Cannot save core from CT:{}: {}", Container, error);
    }

    if (!error) {
        for (auto name = Container; name != "/"; name = TContainer::ParentName(name))
            Conn.IncLabel(name, "CORE.dumped");
    }

    return error;
}

TError TCore::Identify() {
    TStringMap cgmap;
    TError error;

    Container = "/";

    /* all threads except crashed are zombies */
    error = GetTaskCgroups(Tid, cgmap);
    if (!error && !cgmap.count("freezer"))
        error = TError("freezer not found");
    if (error) {
        L_ERR("Cannot get freezer cgroup: {}", error);
        return error;
    }

    auto cg = cgmap["freezer"];
    if (!StringStartsWith(cg, std::string(PORTO_CGROUP_PREFIX) + "/"))
        return TError(EError::InvalidState, "not container");

    Container = cg.substr(strlen(PORTO_CGROUP_PREFIX) + 1);
    Slot = Container.substr(0, Container.find('/'));
    Prefix = StringReplaceAll(Container, "/", "%") + "%";

    //FIXME ugly
    if (Conn.GetProperty(Container, P_CORE_COMMAND, CoreCommand) ||
            Conn.GetProperty(Container, P_USER, User) ||
            Conn.GetProperty(Container, P_GROUP, Group) ||
            Conn.GetProperty(Container, P_OWNER_USER, OwnerUser) ||
            Conn.GetProperty(Container, P_OWNER_GROUP, OwnerGroup) ||
            Conn.GetProperty(Container, P_CWD, Cwd) ||
            Conn.GetProperty(Container, P_STATE, State) ||
            Conn.GetProperty(Container, P_ROOT_PATH, RootPath)) {
        int err;
        std::string msg;
        Conn.GetLastError(err, msg);
        error = TError((EError)err, msg);
        L_ERR("Cannot get CT:{} properties: {}", Container, error);
        return error;
    }

    if (UserId(OwnerUser, OwnerUid))
        OwnerUid = -1;

    if (GroupId(OwnerGroup, OwnerGid))
        OwnerGid = -1;

    return OK;
}

TError TCore::Forward() {
    std::string core = Container + "/core-" + std::to_string(Pid);
    TMultiTuple env = {
        {"CORE_PID", std::to_string(Vpid)},
        {"CORE_TID", std::to_string(Vtid)},
        {"CORE_SIG", std::to_string(Signal)},
        {"CORE_TASK_NAME", ProcessName},
        {"CORE_THREAD_NAME", ThreadName},
        {"CORE_EXE_NAME", ExeName},
        {"CORE_CONTAINER", Container},
        {"CORE_OWNER_UID", std::to_string(OwnerUid)},
        {"CORE_OWNER_GID", std::to_string(OwnerGid)},
        {"CORE_DUMPABLE", std::to_string(Dumpable)},
        {"CORE_ULIMIT", std::to_string(Ulimit)},
        {"CORE_DATETIME", FormatTime(time(nullptr), "%Y%m%dT%H%M%S")},
    };

    /* To let open /dev/stdin */
    fchmod(STDIN_FILENO, 0666);

    if (Conn.CreateWeakContainer(core) ||
            Conn.SetProperty(core, P_ISOLATE, "false") ||
            Conn.SetProperty(core, P_STDIN_PATH, "/dev/fd/0") ||
            Conn.SetProperty(core, P_STDOUT_PATH, "/dev/null") ||
            Conn.SetProperty(core, P_STDERR_PATH, "/dev/null") ||
            Conn.SetProperty(core, P_COMMAND, CoreCommand) ||
            Conn.SetProperty(core, P_USER, User) ||
            Conn.SetProperty(core, P_GROUP, Group) ||
            Conn.SetProperty(core, P_OWNER_USER, OwnerUser) ||
            Conn.SetProperty(core, P_OWNER_GROUP, OwnerGroup) ||
            Conn.SetProperty(core, P_CWD, Cwd) ||
            Conn.SetProperty(core, P_ENV, MergeEscapeStrings(env, '=', ';')))
        return TError("cannot setup CT:{}", core);

    /*
     * Allow poking tasks with suid and ambient capabilities,
     * but ignore error if feature is not supported
     */
    Conn.SetProperty(core, P_CAPABILITIES_AMBIENT, "SYS_PTRACE");

    if (Conn.Start(core))
        return TError("cannot start CT:{}", core);

    L("Forwading core into CT:{}", core);

    std::string result;
    Conn.WaitContainers({core}, {}, result, config().core().timeout_s());
    Conn.Destroy(core);
    return OK;
}

TError TCore::Save() {
    TPath dir = DefaultPattern.DirName();
    std::vector<std::string> names;
    TError error;

    error = dir.ReadDirectory(names);
    if (error)
        return error;

    uint64_t TotalSize = 0;
    uint64_t SlotSize = 0;

    for (auto &name: names) {
        struct stat st;

        if (!(dir / name).StatStrict(st)) {
            TotalSize += st.st_blocks * 512 / st.st_nlink;

            auto sep = name.find('%');
            if (sep != std::string::npos && name.substr(0, sep) == Slot)
                SlotSize += st.st_blocks * 512;
        }

        if ((TotalSize >> 20) >= config().core().space_limit_mb())
            return TError(EError::ResourceNotAvailable,
                    fmt::format("Total core size reached limit: {}M of {}M",
                        TotalSize >> 20, config().core().space_limit_mb()));

        if ((SlotSize >> 20) >= config().core().slot_space_limit_mb())
            return TError(EError::ResourceNotAvailable,
                    fmt::format("Slot {} core size reached limit: {}M of {}M",
                        Slot, SlotSize >> 20, config().core().slot_space_limit_mb()));
    }

    std::string filter = "";
    std::string format = ".core";

    if (StringEndsWith(DefaultPattern.ToString(), ".gz")) {
        filter = "gzip";
        format = ".core.gz";
    }

    if (StringEndsWith(DefaultPattern.ToString(), ".xz")) {
        filter = "xz";
        format = ".core.xz";
    }
    if (StringEndsWith(DefaultPattern.ToString(), ".zst")) {
        filter = "zstd";
        format = ".core.zst";
    }

    Pattern = dir / ( Prefix + ExeName + "." + std::to_string(Pid) +
              ".S" + std::to_string(Signal) + "." +
              FormatTime(time(nullptr), "%Y%m%dT%H%M%S") + format);

    TFile file;
    error = file.Create(Pattern, O_RDWR | O_CREAT | O_EXCL, 0440);
    if (error)
        return error;

    if (Dumpable != 2) {
        error = file.Chown(OwnerUid, OwnerGid);
        if (error)
            L("Cannot chown core: {}", error);
    } /* else owned by root */

    error = DefaultPattern.Hardlink(Pattern);
    if (error) {
        L("Cannot hardlink core to default pattern: {}", error);
        DefaultPattern = "";
    }

    L_CORE("Dumping core into {} ({})", Pattern, DefaultPattern.BaseName());

    off_t size = 0;
    off_t data = 0;
    uint64_t time_ms = GetCurrentTimeMs();

    if (filter.size()) {
        if (file.Fd != STDOUT_FILENO &&
                dup2(file.Fd, STDOUT_FILENO) != STDOUT_FILENO)
            return TError::System("dup2");
        execlp(filter.c_str(), filter.c_str(), nullptr);
        error = TError::System("cannot execute filter " + filter);
    } else {
        uint64_t buf[512];
        off_t sync_start = 0;
        off_t sync_block = config().core().sync_size();

        error = OK;
        do {
            size_t len = 0;

            do {
                ssize_t ret = read(STDIN_FILENO, (uint8_t*)buf + len, sizeof(buf) - len);
                if (ret <= 0) {
                    if (ret < 0)
                        error = TError::System("read");
                    break;
                }
                len += ret;
            } while (len < sizeof(buf));

            bool zero = true;
            for (size_t i = 0; zero && i < sizeof(buf) / sizeof(buf[0]); i++)
                zero = !buf[i];

            if (zero && len == sizeof(buf)) {
                size += len;
                continue;
            }

            size_t off = 0;
            while (off < len) {
                ssize_t ret = pwrite(file.Fd, (uint8_t*)buf + off, len - off, size + off);
                if (ret <= 0) {
                    if (ret < 0)
                        error = TError::System("write");
                    break;
                }
                off += ret;
            }
            size += off;
            data += off;

            if (sync_block && size > sync_start + sync_block * 2) {
                (void)sync_file_range(file.Fd, sync_start, size - sync_start,
                        SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE);
                sync_start = size - sync_block;
            }

            if (off < len || !len)
                break;

        } while (1);

        if (ftruncate(file.Fd, size))
            L("Cannot truncate core dump");
        if (sync_block)
            fdatasync(file.Fd);
    }

    if (!error) {
        time_ms = GetCurrentTimeMs() - time_ms;
        time_ms = time_ms ?: 1;
        L_CORE("Core dump {} ({}) written: {} data, {} holes, {} total, {}B/s",
                Pattern, DefaultPattern.BaseName(), StringFormatSize(data),
                StringFormatSize(size - data), StringFormatSize(size),
                StringFormatSize(data * 1000ull / time_ms));
    } else if (!data) {
        Pattern.Unlink();
        if (DefaultPattern)
            DefaultPattern.Unlink();
    }

    return error;
}
