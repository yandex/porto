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

TError RunCommand(const std::vector<std::string> &command,
                  const TFile &dir, const TFile &in, const TFile &out) {
    TCgroup memcg = MemorySubsystem.Cgroup(PORTO_HELPERS_CGROUP);
    TError error;
    TFile err;
    TTask task;
    TPath path = dir.RealPath();

    if (!command.size())
        return TError("External command is empty");

    error = err.CreateUnnamed("/tmp", O_APPEND);
    if (error)
        return error;

    std::string cmdline;

    for (auto &arg : command)
        cmdline += arg + " ";

    L_ACT("Call helper: {} in {}", cmdline, path);

    error = task.Fork();
    if (error)
        return error;

    if (task.Pid) {
        error = task.Wait();
        if (error) {
            std::string text;
            TError error2 = err.ReadEnds(text, TError::MAX - 1024);
            if (error2)
                text = "Cannot read stderr: " + error2.ToString();
            error = TError(error, "helper: {} stderr: {}", cmdline, text);
        }
        return error;
    }

    error = memcg.Attach(GetPid());
    if (error)
        L_WRN("Cannot attach to helper cgroup: {}", error);

    SetDieOnParentExit(SIGKILL);

    LogFile.Close();
    TFile::CloseAll({dir.Fd, in.Fd, out.Fd, err.Fd});

    if ((in.Fd >= 0 ? dup2(in.Fd, STDIN_FILENO) : open("/dev/null", O_RDONLY)) != STDIN_FILENO)
        _exit(EXIT_FAILURE);

    if (dup2(out.Fd >= 0 ? out.Fd : err.Fd, STDOUT_FILENO) != STDOUT_FILENO)
        _exit(EXIT_FAILURE);

    if (dup2(err.Fd, STDERR_FILENO) != STDERR_FILENO)
        _exit(EXIT_FAILURE);

    TPath root("/");
    TPath dot(".");

    if (dir && !path.IsRoot()) {
        /* Unshare and remount everything except CWD Read-Only */
        if (dir.Chdir() ||
                unshare(CLONE_NEWNS) ||
                root.Remount(MS_PRIVATE | MS_REC) ||
                root.Remount(MS_BIND | MS_REC | MS_RDONLY) ||
                dot.Bind(dot, MS_REC) ||
                TPath("../" + path.BaseName()).Chdir() ||
                dot.Remount(MS_BIND | MS_REC, MS_RDONLY))
            _exit(EXIT_FAILURE);
    } else {
        if (root.Chdir())
            _exit(EXIT_FAILURE);
    }

    const char **argv = (const char **)malloc(sizeof(*argv) * (command.size() + 1));
    for (size_t i = 0; i < command.size(); i++)
        argv[i] = command[i].c_str();
    argv[command.size()] = nullptr;

    execvp(argv[0], (char **)argv);
    _exit(2);
}

TError CopyRecursive(const TPath &src, const TPath &dst) {
    TError error;
    TFile dir;

    error = dir.OpenDir(dst);
    if (error)
        return error;

    return RunCommand({ "cp", "--archive", "--force",
                        "--one-file-system", "--no-target-directory",
                        src.ToString(), "." }, dir);
}

TError ClearRecursive(const TPath &path) {
    TError error;
    TFile dir;

    error = dir.OpenDir(path);
    if (error)
        return error;

    return RunCommand({ "find", ".", "-xdev", "-mindepth", "1", "-delete"}, dir);
}

TError RemoveRecursive(const TPath &path) {
    TError error;
    TFile dir;

    error = dir.OpenDir(path.NormalPath().DirName());
    if (error)
        return error;

    return RunCommand({"rm", "-rf", "--one-file-system", "--", path.ToString()}, dir);
}
