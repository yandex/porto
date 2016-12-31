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

    error = err.CreateTemp("/tmp");
    if (error)
        return error;

    std::string cmdline;

    for (auto &arg : command)
        cmdline += arg + " ";

    L_ACT() << "Call helper: " << cmdline << " in " << cwd << std::endl;

    error = task.Fork();
    if (error)
        return error;

    if (task.Pid) {
        error = task.Wait();
        if (error) {
            std::string msg;
            if (!err.ReadAll(msg, 1024))
                error = TError(error, msg);
        }
        return error;
    }

    error = memcg.Attach(GetPid());
    if (error)
        L_WRN() << "cannot attach to helper cgroup: " << error << std::endl;

    SetDieOnParentExit(SIGKILL);

    TFile::CloseAll({in.Fd, out.Fd, err.Fd});
    if ((in.Fd >= 0 ? dup2(in.Fd, STDIN_FILENO) : open("/dev/null", O_RDONLY)) != STDIN_FILENO)
        _exit(EXIT_FAILURE);
    if ((out.Fd >= 0 ? dup2(out.Fd, STDOUT_FILENO) : open("/dev/null", O_WRONLY)) != STDOUT_FILENO)
        _exit(EXIT_FAILURE);
    if ((err.Fd >= 0 ? dup2(err.Fd, STDERR_FILENO) : open("/dev/null", O_WRONLY)) != STDERR_FILENO)
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

TError ResizeLoopDev(int loopNr, const TPath &image, off_t current, off_t target) {
    auto path = "/dev/loop" + std::to_string(loopNr);
    auto size = std::to_string(target >> 10) + "K";
    TError error;
    TFile dev;

    if (target < current)
        return TError(EError::NotSupported, "Online shrink is not supported yet");

    error = dev.OpenReadWrite(path);
    if (error)
        return error;

    error = image.Truncate(target);
    if (error)
        return error;

    if (ioctl(dev.Fd, LOOP_SET_CAPACITY, 0) < 0)
        return TError(EError::Unknown, errno, "ioctl(LOOP_SET_CAPACITY)");

    return RunCommand({"resize2fs", path, size}, "/");
}
