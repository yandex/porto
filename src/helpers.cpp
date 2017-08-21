#include "helpers.hpp"
#include "common.hpp"
#include "util/path.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/loop.h>
}

TError RunCommand(const std::vector<std::string> &command, const TPath &cwd,
                  const TFile &in, const TFile &out) {
    TCgroup memcg = MemorySubsystem.Cgroup(PORTO_HELPERS_CGROUP);
    TError error;
    TFile err;
    TTask task;

    if (!command.size())
        return TError(EError::Unknown, "External command is empty");

    error = err.CreateUnnamed("/tmp", O_APPEND);
    if (error)
        return error;

    std::string cmdline;

    for (auto &arg : command)
        cmdline += arg + " ";

    L_ACT("Call helper: {} in {}", cmdline, cwd);

    error = task.Fork();
    if (error)
        return error;

    if (task.Pid) {
        error = task.Wait();
        if (error) {
            char buf[2048];
            ssize_t len = pread(err.Fd, buf, sizeof(buf), 0);
            if (len > 0)
                error = TError(error, std::string(buf, len));
        }
        struct stat st;
        if (!err.Stat(st) && st.st_size > 2048)
            L_WRN("Helper {} generated {} bytes in stderr", cmdline, st.st_size);
        return error;
    }

    error = memcg.Attach(GetPid());
    if (error)
        L_WRN("Cannot attach to helper cgroup: {}", error);

    SetDieOnParentExit(SIGKILL);

    TFile::CloseAll({in.Fd, out.Fd, err.Fd});

    if ((in.Fd >= 0 ? dup2(in.Fd, STDIN_FILENO) : open("/dev/null", O_RDONLY)) != STDIN_FILENO)
        _exit(EXIT_FAILURE);

    if (dup2(out.Fd >= 0 ? out.Fd : err.Fd, STDOUT_FILENO) != STDOUT_FILENO)
        _exit(EXIT_FAILURE);

    if (dup2(err.Fd, STDERR_FILENO) != STDERR_FILENO)
        _exit(EXIT_FAILURE);

    /* Remount everything except CWD Read-Only */
    if (!cwd.IsRoot()) {
        std::list<TMount> mounts;
        if (unshare(CLONE_NEWNS) || TPath("/").Remount(MS_PRIVATE | MS_REC) ||
                TPath::ListAllMounts(mounts))
            _exit(EXIT_FAILURE);
        for (auto &mnt: mounts)
            mnt.Target.Remount(MS_REMOUNT | MS_BIND | MS_RDONLY);
        cwd.BindRemount(cwd, 0);
    }

    if (cwd.Chdir())
        _exit(EXIT_FAILURE);

    const char **argv = (const char **)malloc(sizeof(*argv) * (command.size() + 1));
    for (size_t i = 0; i < command.size(); i++)
        argv[i] = command[i].c_str();
    argv[command.size()] = nullptr;

    execvp(argv[0], (char **)argv);
    _exit(2);
}

TError CopyRecursive(const TPath &src, const TPath &dst) {
    return RunCommand({ "cp", "--archive", "--force",
                        "--one-file-system", "--no-target-directory",
                        src.ToString(), "." }, dst);
}

TError ClearRecursive(const TPath &path) {
    return RunCommand({ "find", ".", "-xdev", "-mindepth", "1", "-delete"}, path);
}

TError RemoveRecursive(const TPath &path) {
    return RunCommand({"rm", "-rf", "--one-file-system", "--", path.ToString()},
                      path.NormalPath().DirName());
}
