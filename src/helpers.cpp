#include "helpers.hpp"
#include "common.hpp"
#include "client.hpp"
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

extern std::atomic_bool NeedStopHelpers;

static void HelperError(TFile &err, const std::string &text, TError error) __attribute__ ((noreturn));

static void HelperError(TFile &err, const std::string &text, TError error) {
    L_WRN("{}: {}", text, error);
    err.WriteAll(fmt::format("{}: {}", text, error));
    _exit(EXIT_FAILURE);
}

TError RunCommand(const std::vector<std::string> &command,
                  const TFile &dir, const TFile &in, const TFile &out,
                  const TCapabilities &caps,
                  bool verboseError,
                  bool interruptible) {
    return RunCommand(command, {}, dir, in, out, caps, PORTO_HELPERS_CGROUP, verboseError, interruptible);
}

TError RunCommand(const std::vector<std::string> &command,
                  const std::vector<std::string> &env,
                  const TFile &dir, const TFile &in, const TFile &out,
                  const TCapabilities &caps,
                  const std::string &memCgroup,
                  bool verboseError,
                  bool interruptible) {
    TCgroup memcg = MemorySubsystem.Cgroup(memCgroup);
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
        if (interruptible && CL)
            error = task.Wait(interruptible, NeedStopHelpers, CL->Closed);
        else
            error = task.Wait(interruptible, NeedStopHelpers);

        if (error && error == EError::Unknown) {
            std::string text;
            TError error2 = err.ReadEnds(text, TError::MAX_LENGTH - 1024);
            if (error2)
                text = "Cannot read stderr: " + error2.ToString();

            if (verboseError) {
                error.Error = EError::HelperError;
                if (text.find("not recoverable") != std::string::npos)
                    error.Error = EError::HelperFatalError;
            }

            error = TError(error, "helper: {} stderr: {}", cmdline, text);
        }
        return error;
    }

    SetProcessName("portod-" + command[0]);

    error = memcg.Attach(GetPid());
    if (error)
        HelperError(err, "Cannot attach to helper cgroup: {}", error);

    SetDieOnParentExit(SIGKILL);

    if (!in) {
        TFile in;
        error = in.Open("/dev/null", O_RDONLY);
        if (error)
            HelperError(err, "open stdin", error);
        if (dup2(in.Fd, STDIN_FILENO) != STDIN_FILENO)
            HelperError(err, "stdin", TError::System("dup2"));
    } else {
        if (dup2(in.Fd, STDIN_FILENO) != STDIN_FILENO)
            HelperError(err, "stdin", TError::System("dup2"));
    }

    if (dup2(out ? out.Fd : err.Fd, STDOUT_FILENO) != STDOUT_FILENO)
        HelperError(err, "stdout", TError::System("dup2"));

    if (dup2(err.Fd, STDERR_FILENO) != STDERR_FILENO)
        HelperError(err, "stderr", TError::System("dup2"));

    TPath root("/");
    TPath dot(".");

    if (dir && !path.IsRoot()) {
        /* Unshare and remount everything except CWD Read-Only */
        error = dir.Chdir();
        if (error)
            HelperError(err, "chdir", error);

        if (unshare(CLONE_NEWNS))
            HelperError(err, "newns", TError::System("unshare"));

        error = root.Remount(MS_PRIVATE | MS_REC);
        if (error)
            HelperError(err, "remont", error);

        error = root.Remount(MS_BIND | MS_REC | MS_RDONLY);
        if (error)
            HelperError(err, "remont", error);

        error = dot.Bind(dot, MS_REC);
        if (error)
            HelperError(err, "bind", error);

        error = TPath("../" + path.BaseName()).Chdir();
        if (error)
            HelperError(err, "chdir bind", error);

        error = dot.Remount(MS_BIND | MS_REC | MS_ALLOW_WRITE);
        if (error)
            HelperError(err, "remount bind", error);
    } else {
        error = root.Chdir();
        if (error)
            HelperError(err, "root chdir", error);
    }

    error = caps.ApplyLimit();
    if (error)
        HelperError(err, "caps", error);

    TFile::CloseAllExcept({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO});

    const char **argv = (const char **)malloc(sizeof(*argv) * (command.size() + 1));
    for (size_t i = 0; i < command.size(); i++)
        argv[i] = command[i].c_str();
    argv[command.size()] = nullptr;

    if (env.size() == 0) {
        execvp(argv[0], (char **)argv);
    } else {
        const char **envp = (const char **)malloc(sizeof(*envp) * (env.size() + 1));
        for (size_t i = 0; i < env.size(); i++)
            envp[i] = env[i].c_str();
        envp[env.size()] = nullptr;

        execvpe(argv[0], (char **)argv, (char **) envp);
    }

    err.SetFd = STDERR_FILENO;
    HelperError(err, fmt::format("Cannot execute {}", argv[0]), TError::System("exec"));
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

TError RemoveRecursive(const TPath &path, bool interruptible) {
    TError error;
    TFile dir;

    error = dir.OpenDir(path.NormalPath().DirName());
    if (error)
        return error;

    return RunCommand({"rm", "-rf", "--one-file-system", "--", path.ToString()}, dir, TFile(), TFile(), HelperCapabilities, false, interruptible);
}
