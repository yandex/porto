#include "batch.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

TError TBatchTask::RunSync(TContext &context) {
    TError error = Task();
    if (error)
        L_ERR() << "Batch task returned: " << error << std::endl;
    PostHook(error);
    return error;
}

TError TBatchTask::Run(TContext &context) {
    if (config().daemon().sync_batch())
        return RunSync(context);
    else
        return RunAsync(context);
}

TError TBatchTask::RunAsync(TContext &context) {
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
        context.Posthooks[pid] = PostHook;
        context.PosthooksError[pid] = pfd[0];
    }

    return TError::Success();
}
