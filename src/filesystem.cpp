#include "filesystem.hpp"
#include "config.hpp"
#include "util/log.hpp"


extern "C" {
#include <sys/stat.h>
#include <linux/kdev_t.h>
}

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

    return normal.IsRoot() || normal.IsInside(SystemPaths);
}

TError TMountNamespace::MountBinds() {
    for (const auto &bm : BindMounts) {
        bool ro = bm.ReadOnly;
        TPath src, dest;
        TError error;

        if (bm.Source.IsAbsolute())
            src = bm.Source;
        else
            src = ParentCwd / bm.Source;

        if (bm.Dest.IsAbsolute())
            dest = Root / bm.Dest;
        else
            dest = Root / Cwd / bm.Dest;

        if (!StringStartsWith(dest.RealPath().ToString(), Root.ToString()))
            return TError(EError::InvalidValue, "Bind mount target " + dest.ToString() +
                    " outside of container root " + Root.ToString());

        if (!src.Exists())
            return TError(EError::InvalidValue, "Bind mount source does not exist " + src.ToString());

        /*
         * ReadOnly  -> ro
         * ReadWrite -> rw
         * neither   -> rw if have write permissions
         */
        if (!src.HasAccess(OwnerCred, ro ? TPath::RU : TPath::RWU)) {
            bool sysPath = IsSystemPath(src);

            if (config().privileges().enforce_bind_permissions() || sysPath) {
                if (!sysPath && !ro && !bm.ReadWrite && src.HasAccess(OwnerCred, TPath::RU))
                    ro = true;
                else
                    return TError(EError::Permission, "User " + OwnerCred.ToString() +
                            " have not enough permissions for bind mount source " + src.ToString());
            } else
                L_WRN() << Container << ": User " << OwnerCred.ToString() <<
                    " have not enough permissions for bind mount source " + src.ToString() << std::endl;
        }

        if (dest.Exists()) {
            if (!dest.HasAccess(OwnerCred, TPath::WU)) {
                if (config().privileges().enforce_bind_permissions() ||
                        IsSystemPath(dest))
                    return TError(EError::Permission, "User " + OwnerCred.ToString() +
                            " have no write permissions for bind mount target " + dest.ToString());
                else
                    L_WRN() << Container << ": User " << OwnerCred.ToString() <<
                        " have no write permissions for bind mount target " + dest.ToString() << std::endl;
            }
            if (src.IsDirectoryFollow() != dest.IsDirectoryFollow())
                return TError(EError::InvalidProperty,
                        "Bind mount source and target must be both file or directory");
        } else {
            if (src.IsDirectoryFollow())
                error = dest.MkdirAll(0755);
            else
                error = dest.CreateAll(0600);
            if (!error)
                error = dest.Chown(OwnerCred);
            if (error)
                return error;
        }

        // Drop nosuid,noexec,nodev
        error = dest.BindRemount(src, ro ? MS_RDONLY : 0);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TMountNamespace::RemountRootRo() {
    if (!RootRdOnly || LoopDev >= 0)
        return TError::Success();

    // remount everything except binds to ro
    std::vector<std::shared_ptr<TMount>> snapshot;
    TError error = TMount::Snapshot(snapshot);
    if (error)
        return error;

    for (auto mnt : snapshot) {
        TPath path = Root.InnerPath(mnt->GetMountpoint());
        if (path.IsEmpty())
            continue;

        bool skip = false;
        for (const auto &bm : BindMounts) {
            if (bm.Dest.NormalPath() == path.NormalPath()) {
                skip = true;
                break;
            }
        }
        if (skip)
            continue;

        error = mnt->GetMountpoint().Remount(MS_REMOUNT | MS_BIND | MS_RDONLY);
        if (error)
            return error;
    }

    return TError::Success();
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
        return TError(EError::Unknown, "Too many subdirectories in /run!");

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
            return TError(EError::Unknown, "Too many subdirectories in /run!");

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
                { "mode=755", "size=32m"});
    if (error)
        return error;

    for (unsigned int i = 0; i < run_paths.size(); i++) {
        TPath current = run / run_paths[i];
        auto &current_stat = run_paths_stat[i];
        mode_t mode = current_stat.st_mode;

        /* forbid other-writable directory without sticky bit */
        if ((mode & 01002) == 02) {
            L() << "Other writable without sticky: " << current << std::endl;
            mode &= ~02;
        }

        error = current.Mkdir(mode);
        if (error)
            return error;

        error = current.Chown(current_stat.st_uid, current_stat.st_gid);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TMountNamespace::BindResolvConf() {
    std::vector<std::string> files = { "/etc/hosts", "/etc/resolv.conf" };

    for (auto &file : files) {
        TPath path = Root / file;
        TError error = path.CreateAll(0600);
        if (!error)
            error = path.BindRemount(file, MS_RDONLY);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TMountNamespace::MountRootFs() {
    TError error;

    if (Root.IsRoot())
        return TError::Success();

    if (LoopDev >= 0)
        error = Root.Mount("/dev/loop" + std::to_string(LoopDev),
                                "ext4", RootRdOnly ? MS_RDONLY : 0, {});
    else
        error = Root.Bind(Root);
    if (error)
        return error;

    struct {
        std::string target;
        std::string type;
        unsigned long flags;
        std::vector<std::string> opts;
    } mounts[] = {
        { "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME,
            { "mode=755", "size=32m" } },
        { "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
            { "newinstance", "ptmxmode=0666", "mode=620" ,"gid=5" }},
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
    };

    for (auto &d : dirs) {
        error = (Root + d.path).Mkdir(d.mode);
        if (error)
            return error;
    }

    struct {
        const std::string path;
        mode_t mode;
        dev_t dev;
    } nodes[] = {
        { "/dev/null",    0666 | S_IFCHR, MKDEV(1, 3) },
        { "/dev/zero",    0666 | S_IFCHR, MKDEV(1, 5) },
        { "/dev/full",    0666 | S_IFCHR, MKDEV(1, 7) },
        { "/dev/random",  0666 | S_IFCHR, MKDEV(1, 8) },
        { "/dev/urandom", 0666 | S_IFCHR, MKDEV(1, 9) },
        { "/dev/tty",     0666 | S_IFCHR, MKDEV(5, 0) },
        { "/dev/console", 0600 | S_IFCHR, MKDEV(1, 3) }, /* to /dev/null */
    };

    for (auto &n : nodes) {
        error = (Root + n.path).Mknod(n.mode, n.dev);
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
        { "/dev/shm", "../run/shm" },
    };

    for (auto &s : symlinks) {
        error = (Root + s.path).Symlink(s.target);
        if (error)
            return error;
    }

    std::vector<std::string> proc_ro = {
        "/proc/sysrq-trigger",
        "/proc/irq",
        "/proc/bus",
    };

    if (!OwnerCred.IsRootUser())
        proc_ro.push_back("/proc/sys");

    for (auto &p : proc_ro) {
        TPath path = Root + p;
        error = path.BindRemount(path, MS_RDONLY);
        if (error)
            return error;
    }

    TPath proc_kcore = Root + "/proc/kcore";
    error = proc_kcore.BindRemount(Root + "/dev/null", MS_RDONLY);
    if (error)
        return error;

    if (BindDns) {
        error = BindResolvConf();
        if (error)
            return error;
    } else if (ResolvConf.length()) {
        TPath resolvconf = Root + "/etc/resolv.conf";
        if (!resolvconf.IsRegularStrict()) {
            if (!resolvconf.Exists())
                error = resolvconf.Mkfile(0644);
            else
                error = TError(EError::InvalidState, "non-regular file");
        }
        if (!error)
            error = resolvconf.WriteAll(ResolvConf);
        if (error)
            return TError(error, "cannot write /etc/resolv.conf");
    }

    return TError::Success();
}

TError TMountNamespace::IsolateFs() {

    if (Root.IsRoot())
        return TError::Success();

    TError error = Root.PivotRoot();
    if (error) {
        L_WRN() << "Can't pivot root, roll back to chroot: " << error << std::endl;

        error = Root.Chroot();
        if (error)
            return error;
    }

    // Allow suid binaries and device nodes at container root.
    error = TPath("/").Remount(MS_REMOUNT | MS_BIND |
                               (RootRdOnly ? MS_RDONLY : 0));
    if (error) {
        L_ERR() << "Can't remount / as suid and dev:" << error << std::endl;
        return error;
    }

    TPath newRoot("/");
    return newRoot.Chdir();
}
