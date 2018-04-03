#include "filesystem.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "util/log.hpp"


extern "C" {
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <linux/kdev_t.h>
#include <sys/vfs.h>
#include <linux/magic.h>
}

#ifndef TRACEFS_MAGIC
#define TRACEFS_MAGIC          0x74726163
#endif

static std::vector<TPath> SystemPaths = {
    "/bin",
    "/boot",
    "/dev",
    "/etc",
    "/lib",
    "/lib32",
    "/lib64",
    "/libx32",
    "/proc",
    "/root",
    "/sbin",
    "/sys",
    "/usr",
    "/var",
};

bool IsSystemPath(const TPath &path) {
    TPath normal = path.NormalPath();

    if (normal.IsRoot())
        return true;

    if (normal == "/home")
        return true;

    for (auto &sys: SystemPaths)
        if (normal.IsInside(sys))
            return true;

    return false;
}

TError TBindMount::Parse(const std::string &str, std::vector<TBindMount> &binds) {
    auto lines = SplitEscapedString(str, ' ', ';');
    TError error;

    for (auto &line: lines) {
        if (line.size() < 2)
            return TError(EError::InvalidValue, "Invalid bind mount {}", str);

        TBindMount bind;

        bind.Source = line[0];
        bind.Target = line[1];

        bind.CreateTarget = true;
        bind.FollowTraget = true;

        if (line.size() > 2) {
            uint64_t flags;
            error = TMount::ParseFlags(line[2], flags,
                                       MS_RDONLY | MS_ALLOW_WRITE |
                                       MS_NODEV | MS_ALLOW_DEV |    /* check permissions at start */
                                       MS_NOSUID | MS_ALLOW_SUID |
                                       MS_NOEXEC | MS_ALLOW_EXEC |
                                       MS_REC | MS_PRIVATE | MS_UNBINDABLE |
                                       MS_NOATIME | MS_NODIRATIME | MS_RELATIME);
            if (error)
                return error;

            // by default disable backward propagation
            if (!(flags & (MS_PRIVATE | MS_UNBINDABLE)))
                flags |= MS_SLAVE | MS_SHARED;

            // FIXME temporary hack
            for (auto &src: config().container().rec_bind_hack())
                if (bind.Source == src)
                    flags |= MS_REC;

            bind.MntFlags = flags;
        }

        binds.push_back(bind);
    }

    return OK;
}

std::string TBindMount::Format(const std::vector<TBindMount> &binds) {
    TMultiTuple lines;
    for (auto &bind: binds)
        lines.push_back({bind.Source.ToString(), bind.Target.ToString(),
                         TMount::FormatFlags(bind.MntFlags & ~(MS_SLAVE | MS_SHARED))});
    return MergeEscapeStrings(lines, ' ', ';');
}

TError TBindMount::Mount(const TCred &cred, const TPath &root) const {
    bool directory = IsDirectory;
    TFile src, dst;
    TError error;

    error = src.OpenPath(Source);
    if (error)
        return error;

    if (!IsDirectory && !IsFile)
        directory = Source.IsDirectoryFollow();

    if (!ControlSource) {
        /* not read-only means read-write, protect system directories from dac override */
        if (!(MntFlags & MS_RDONLY) || (directory && IsSystemPath(Source)))
            error = src.WriteAccess(cred);
        else
            error = src.ReadAccess(cred);
        if (error)
            return TError(error, "Bindmount {}", Target);
    }

    if (CreateTarget && !Target.Exists()) {
        TPath base = Target.DirName();
        std::list<std::string> dirs;
        TFile dir;

        while (!base.Exists()) {
            dirs.push_front(base.BaseName());
            base = base.DirName();
        }

        error = dir.OpenDir(base);
        if (error)
            return error;

        TPath real = dir.RealPath();
        if (!FollowTraget && base != real)
            return TError(EError::InvalidValue, "Real target path differs: {} -> {}", base, real);

        if (!root.IsRoot() && !real.IsInside(root))
            return TError(EError::InvalidValue, "Bind mount target out of chroot: {} -> {}", base, real);

        if (root.IsRoot() && !ControlTarget)
            error = dir.WriteAccess(cred);

        for (auto &name: dirs) {
            if (!error)
                error = dir.MkdirAt(name, 0775);
            if (!error)
                error = dir.WalkStrict(dir, name);
            if (!error)
                error = dir.Chown(cred);
        }

        if (error)
            return TError(error, "Bindmount {}", Target);

        if (directory) {
            error = dir.MkdirAt(Target.BaseName(), 0775);
            if (!error)
                error = dst.OpenDir(Target);
        } else
            error = dst.OpenAt(dir, Target.BaseName(), O_CREAT | O_WRONLY | O_CLOEXEC |
                               (FollowTraget ? 0 : O_NOFOLLOW), 0664);
        if (!error)
            error = dst.Chown(cred);
    } else {
        if (directory)
            error = dst.OpenDir(Target);
        else
            error = dst.OpenRead(Target);

        // do not override non-writable directies in host or system directories
        if (!error && !ControlTarget && (root.IsRoot() || IsSystemPath(Target)))
            error = dst.WriteAccess(cred);
    }
    if (error)
        return TError(error, "Bindmount {}", Target);

    TPath real = dst.RealPath();

    if (!FollowTraget && Target != real)
        return TError(EError::InvalidValue, "Real target path differs: {} -> {}", Target, real);

    if (!root.IsRoot() && !real.IsInside(root))
        return TError(EError::InvalidValue, "Bind mount target out of chroot: {} -> {}", Target, real);

    error = dst.ProcPath().Bind(src.ProcPath(), MntFlags & MS_REC);
    if (error)
        return error;

    error = Target.Remount(MS_BIND | MntFlags);
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::MountRun() {
    TPath run = Root / "run";
    std::vector<std::string> run_paths, subdirs;
    std::vector<struct stat> run_paths_stat;
    TError error;

    if (run.Exists()) {
        error = run.ListSubdirs(subdirs);
        if (error)
            return error;
    }

    /* We want to recreate /run dir tree with up to RUN_SUBDIR_LIMIT nodes */
    if (subdirs.size() >= RUN_SUBDIR_LIMIT)
        return TError("Too many subdirectories in /run!");

    run_paths.reserve(RUN_SUBDIR_LIMIT);
    for (const auto &i: subdirs) {

        /* Skip creating special directories, we'll do it later */
        if (i == "shm" || i == "lock")
            continue;

        run_paths.push_back(i);
    }

    for (auto i = run_paths.begin(); i != run_paths.end(); ++i) {
        auto current = *i;
        TPath current_path = run / current;

        error = current_path.ListSubdirs(subdirs);
        if (error)
            return error;

        if (subdirs.size() + run_paths.size() >= RUN_SUBDIR_LIMIT)
            return TError("Too many subdirectories in /run!");

        for (auto dir : subdirs)
            run_paths.push_back(current + "/" + dir);
    }

    for (const auto &i: run_paths) {
        struct stat current_stat;
        TPath current_path = run / i;

        error = current_path.StatStrict(current_stat);
        if (error)
            return error;

        run_paths_stat.push_back(current_stat);
    }

    error = run.MkdirAll(0755);
    if (!error)
        error = run.Mount("tmpfs", "tmpfs",
                MS_NOSUID | MS_NODEV | MS_STRICTATIME,
                { "mode=755", "size=" + std::to_string(RunSize) });
    if (error)
        return error;

    for (unsigned int i = 0; i < run_paths.size(); i++) {
        TPath current = run / run_paths[i];
        auto &current_stat = run_paths_stat[i];
        mode_t mode = current_stat.st_mode;

        /* forbid other-writable directory without sticky bit */
        if ((mode & 01002) == 02) {
            L("Other writable without sticky: {}", current);
            mode &= ~02;
        }

        error = current.Mkdir(mode);
        if (error)
            return error;

        error = current.Chown(current_stat.st_uid, current_stat.st_gid);
        if (error)
            return error;
    }

    return OK;
}

TError TMountNamespace::MountTraceFs() {
    TError error;

    TPath tracefs = "/sys/kernel/tracing";
    if (!config().container().enable_tracefs() || !tracefs.Exists())
        return OK;

    struct statfs st;
    if (statfs(tracefs.c_str(), &st) || st.f_type != TRACEFS_MAGIC)
        return TError(EError::Unknown, "Tracefs is not mounted");

    /* read-only bind instead for new mount to preserve read-write in host */
    error = (Root / tracefs).BindRemount(tracefs, MS_RDONLY);
    if (error)
        return error;

    TPath debugfs = Root / "/sys/kernel/debug";
    if (debugfs.Exists()) {
        TPath tracing = debugfs / "tracing";
        error = debugfs.Mount("none", "tmpfs", 0, {"mode=755", "size=0"});
        if (!error)
            error = tracing.Mkdir(0700);
        if (!error)
            error = tracing.BindRemount(tracefs, MS_RDONLY);
        if (!error)
            error = debugfs.Remount(MS_RDONLY);
        if (error)
            return error;
    }

    return OK;
}

TError TMountNamespace::MountSystemd() {

    if (Systemd.empty())
        return OK;

    TPath tmpfs = Root / "sys/fs/cgroup";
    TPath systemd = tmpfs / "systemd";
    TPath systemd_rw = systemd / Systemd;
    TError error;

    error = tmpfs.Mount("tmpfs", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV | MS_STRICTATIME, {"mode=755"});
    if (!error)
        error = systemd.MkdirAll(0755);
    if (!error)
        error = tmpfs.Remount(MS_RDONLY);
    if (!error)
        error = systemd.Mount("cgroup", "cgroup", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, { "name=systemd" });
    if (!error)
        error = systemd_rw.BindRemount(systemd_rw, MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_ALLOW_WRITE);

    return error;
}

TError TMountNamespace::SetupRoot() {
    TError error;

    if (!Root.Exists())
        return TError(EError::InvalidValue, "Root path does not exist");

    struct {
        std::string target;
        std::string type;
        unsigned long flags;
        std::vector<std::string> opts;
    } mounts[] = {
        { "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME,
            { "mode=755", "size=" + std::to_string(config().container().dev_size()) }},
        { "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
            { "newinstance", "ptmxmode=0666", "mode=620" ,"gid=5",
              "max=" + std::to_string(config().container().devpts_max()) }},
        { "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, {}},
        { "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, {}},
    };

    for (auto &m : mounts) {
        TPath target = Root + m.target;
        error = target.MkdirAll(0755);
        if (!error)
            error = target.Mount(m.type, m.type, m.flags, m.opts);
        if (error)
            return error;
    }

    error = MountRun();
    if (error)
        return error;

    if (BindPortoSock) {
        TPath sock(PORTO_SOCKET_PATH);
        TPath dest = Root / sock;

        error = dest.Mkfile(0);
        if (error)
            return error;
        error = dest.Bind(sock);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        mode_t mode;
    } dirs[] = {
        { "/run/lock",  01777 },
        { "/run/shm",   01777 },
        { "/dev/shm",   01777 },
    };

    for (auto &d : dirs) {
        error = (Root + d.path).Mkdir(d.mode);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        const std::string target;
    } symlinks[] = {
        { "/dev/ptmx", "pts/ptmx" },
        { "/dev/fd", "/proc/self/fd" },
        { "/dev/stdin", "/proc/self/fd/0" },
        { "/dev/stdout", "/proc/self/fd/1" },
        { "/dev/stderr", "/proc/self/fd/2" },
    };

    for (auto &s : symlinks) {
        error = (Root + s.path).Symlink(s.target);
        if (error)
            return error;
    }

    if (HugetlbSubsystem.Supported) {
        TPath path = Root + "/dev/hugepages";
        error = path.Mkdir(0755);
        if (error)
            return error;
        error = path.Mount("hugetlbfs", "hugetlbfs", MS_NOSUID | MS_NODEV, { "mode=01777" });
        if (error)
            return error;
    }

    struct {
        std::string dst;
        std::string src;
        unsigned long flags;
    } binds[] = {
        { "/run/lock", "/run/lock", MS_NOSUID | MS_NODEV | MS_NOEXEC },
        { "/dev/shm", "/run/shm", MS_NOSUID | MS_NODEV | MS_STRICTATIME },
    };

    for (auto &b : binds) {
        TPath dst = Root + b.dst;
        TPath src = Root + b.src;

        error = dst.BindRemount(src, b.flags);
        if (error)
            return error;
    }

    error = MountTraceFs();
    if (error)
        L_WRN("Cannot mount tracefs: {}", error);

    error = MountSystemd();
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::ProtectProc() {
    TError error;

    std::vector<TPath> proc_ro = {
        "/proc/sysrq-trigger",
        "/proc/irq",
        "/proc/bus",
        "/proc/sys",
    };

    for (auto &path : proc_ro) {
        error = path.BindRemount(path, MS_RDONLY);
        if (error)
            return error;
    }

    TPath proc_kcore("/proc/kcore");
    error = proc_kcore.BindRemount("/dev/null", MS_RDONLY);
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::Setup() {
    TPath root("/"), proc("/proc");
    TFile rootFd;
    TError error;

    // remount as slave to receive propagations from parent namespace
    error = root.Remount(MS_SLAVE | MS_REC);
    if (error)
        return error;

    error = rootFd.OpenDir(Root);
    if (error)
        return error;

    /* new root must be a different mount */
    if (!Root.IsRoot() && rootFd.GetMountId() == rootFd.GetMountId("..")) {
        error = Root.Bind(Root, MS_REC);
        if (error)
            return error;
        error = rootFd.OpenDir(Root);
        if (error)
            return error;
    }

    // allow suid binaries at root volume
    if (!Root.IsRoot()) {
        error = Root.Remount(MS_BIND | MS_ALLOW_SUID);
        if (error)
            return error;
    }

    if (RootRo) {
        error = Root.Remount(MS_BIND | MS_REC | MS_RDONLY);
        if (error)
            return error;
    }

    // mount proc so PID namespace works
    error = proc.UmountAll();
    if (error)
        return error;

    error = proc.Mount("proc", "proc", MS_NOEXEC | MS_NOSUID | MS_NODEV, {});
    if (error)
        return error;

    if (Root.IsRoot()) {
        error = TPath("/sys/fs/cgroup").UmountAll();
        if (error)
            return error;

        error = TPath("/sys/fs/pstore").UmountAll();
        if (error)
            return error;

        error = TPath("/sys/kernel/security").UmountAll();
        if (error)
            return error;

        error = TPath(PORTO_CONTAINERS_KV).UmountAll();
        if (error)
            return error;

        error = TPath(PORTO_VOLUMES_KV).UmountAll();
        if (error)
            return error;

        // protect sysfs
        error = TPath("/sys").Remount(MS_BIND | MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_REC);
        if (error)
            return error;
    } else {
        error = SetupRoot();
        if (error)
            return error;
    }

    for (const auto &bind : BindMounts) {
        error = bind.Mount(BindCred, Root);
        if (error)
            return error;
    }

    if (!Root.IsRoot()) {
        error = rootFd.PivotRoot();
        if (error) {
            L_WRN("Cannot pivot root, roll back to chroot: {}", error);
            error = Root.Chroot();
            if (error)
                return error;
        }

        error = root.Chdir();
        if (error)
            return error;
    }

    for (const auto &link: Symlink) {
        error = CreateSymlink(link.first, link.second);
        if (error)
            return error;
    }

    // remount as shared: subcontainers will get propgation from us
    error = root.Remount(MS_SHARED | MS_REC);
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::Enter(pid_t pid) {
    TError error;

    error = HostNs.Open("/proc/thread-self/ns/mnt");
    if (error)
        return error;

    error = ContainerNs.Open(pid, "ns/mnt");
    if (error)
        return error;

    if (unshare(CLONE_FS))
        return TError::System("unshare(CLONE_FS)");

    error = ContainerNs.SetNs(CLONE_NEWNS);
    if (error)
        return error;

    error = TPath("/").Chdir();
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::Leave() {
    TError error;

    error = HostNs.SetNs(CLONE_NEWNS);
    if (error)
        return error;

    error = TPath("/").Chdir();
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::CreateSymlink(const TPath &symlink, const TPath &target) {
    TPath sym = symlink.AbsolutePath(Cwd).NormalPath();
    TPath sym_dir = sym.DirNameNormal();
    TPath sym_name = sym.BaseNameNormal();
    TError error;
    TFile dir;

    // in chroot allow to change anything
    error = dir.CreatePath(sym_dir, BindCred, Root.IsRoot() ? "" : "/");
    if (error)
        return error;

    TPath tgt = target.AbsolutePath(Cwd).NormalPath().RelativePath(sym_dir);
    TPath cur_tgt;

    if (!dir.ReadlinkAt(sym_name, cur_tgt)) {
        if (!target) {
            L_ACT("symlink {} remove {}", sym, cur_tgt);
            error = dir.UnlinkAt(sym_name);
        } else if (cur_tgt == tgt) {
            L_ACT("symlink {} already points to {}", sym, tgt);
        } else {
            L_ACT("symlink {} replace {} with {}", sym, cur_tgt, tgt);
            TPath sym_next = ".next_" + sym_name.ToString();
            (void)dir.UnlinkAt(sym_next);
            error = dir.SymlinkAt(sym_next, tgt);
            if (!error)
                error = dir.ChownAt(sym_next, BindCred);
            if (!error)
                error = dir.RenameAt(sym_next, sym_name);
        }
    } else {
        L_ACT("symlink {} to {}", sym, tgt);
        error = dir.SymlinkAt(sym_name, tgt);
        if (!error)
            error = dir.ChownAt(sym_name, BindCred);
    }

    return error;
}
