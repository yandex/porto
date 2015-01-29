#include "batch.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

TError TBatchTask::Run(TContext &context) {
    int pfd[2];
    int ret = pipe2(pfd, O_CLOEXEC);
    if (ret)
        return TError(EError::Unknown, "pipe2(batch)");

    int pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return TError(EError::Unknown, "fork(batch)");
    } else if (pid == 0) {
        close(pfd[0]);
        /* Child */
        CloseAllFds();
        SetProcessName("portod-batch");
        SetDieOnParentExit();
        TLogger::DisableLog();
        TError error = Task();
        (void)error.Serialize(pfd[1]);
        exit(error);
    } else {
        close(pfd[1]);
        /* Parent */
        context.Posthooks[ret] = PostHook;
        context.Errors[ret] = pfd[0];
    }

    return TError::Success();
}
