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
    TFile outFd;

    pid_t pid = fork();
    if (pid < 0)
        return TError(EError::Unknown, errno, "RunCommand: fork");

    if (output) {
        error = outFd.CreateTemp("/tmp");
        if (error)
            return error;
    }

    if (pid > 0) {
        int ret, status;
retry:
        ret = waitpid(pid, &status, 0);
        if (ret < 0) {
            if (errno == EINTR)
                goto retry;
            return TError(EError::Unknown, errno, "RunCommand: waitpid");
        }
        if (output)
            outFd.ReadAll(*output, 4096);
        if (WIFEXITED(status) && !WEXITSTATUS(status))
            return TError::Success();
        return TError(EError::Unknown, "RunCommand: " + command[0] +
                                       " " + FormatExitStatus(status));
    }

    error = memcg.Attach(GetPid());
    if (error)
        L_WRN() << "cannot attach to helper cgroup: " << error << std::endl;

    SetDieOnParentExit(SIGKILL);

    TFile::CloseAll({outFd.Fd});
    if (open("/dev/null", O_RDONLY) < 0 ||
            open("/dev/null", O_WRONLY) < 0 ||
            open("/dev/null", O_WRONLY) < 0)
        _exit(EXIT_FAILURE);
    if (output && dup2(outFd.Fd, STDOUT_FILENO) != STDOUT_FILENO)
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
