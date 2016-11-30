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
                  std::string *output) {
    TCgroup memcg = MemorySubsystem.Cgroup(PORTO_HELPERS_CGROUP);
    TError error;
    TFile outFd, errFd;
    TTask task;

    if (!command.size())
        return TError(EError::Unknown, "External command is empty");

    if (output) {
        error = outFd.CreateTemp("/tmp");
        if (error)
            return error;
    }

    error = errFd.CreateTemp("/tmp");
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
            std::string error_log;

            (void)errFd.ReadTail(error_log, 4096);

            L_WRN() << "External helper command: " << command[0] << " failed, stderr : "
                    << std::endl << error_log << std::endl;

        } else if (output)
            error = outFd.ReadTail(*output, 4096);

        return error;
    }

    error = memcg.Attach(GetPid());
    if (error)
        L_WRN() << "cannot attach to helper cgroup: " << error << std::endl;

    SetDieOnParentExit(SIGKILL);

    TFile::CloseAll({outFd.Fd, errFd.Fd});
    if (open("/dev/null", O_RDONLY) < 0 ||
            open("/dev/null", O_WRONLY) < 0 ||
            open("/dev/null", O_WRONLY) < 0)
        _exit(EXIT_FAILURE);
    if (output && dup2(outFd.Fd, STDOUT_FILENO) != STDOUT_FILENO)
        _exit(EXIT_FAILURE);
    if (dup2(errFd.Fd, STDERR_FILENO) != STDERR_FILENO)
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

TError PackTarball(const TPath &tar, const TPath &path) {
    return RunCommand({ "tar", "--one-file-system", "--numeric-owner",
                        "--sparse",  "--transform", "s:^./::",
                        "-cpaf", tar.ToString(),
                        "-C", path.ToString(), "." }, tar.DirName());
}

TError UnpackTarball(const TPath &tar, const TPath &path) {
    return RunCommand({ "tar", "--numeric-owner", "-pxf", tar.ToString()}, path);
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
