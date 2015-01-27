#pragma once

#include <map>

#include "util/unix.hpp"
#include "client.hpp"
#include "common.hpp"
#include "context.hpp"

class TBatchTask : public TNonCopyable {
public:
    TBatchTask(task_t task, posthook_t post)
        : Task(task), PostHook(post) {};

    TError Run(TContext &context) {

        int ret = fork();
        switch (ret) {
        case -1:
            return TError(EError::Unknown, "");

        case 0:
            /* Child */
            CloseAllFds();
            SetDieOnParentExit();
            exit(Task());
            break;

        default:
            /* Parent */
            context.Posthooks[ret] = PostHook;
        }

        return TError::Success();
    }

private:
    task_t Task;
    posthook_t PostHook;
};
