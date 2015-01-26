#pragma once

#include <map>

#include "util/unix.hpp"
#include "client.hpp"
#include "common.hpp"
#include "context.hpp"

typedef std::function<int()> task_t;
typedef std::function<void(int ret)> posthook_t;

extern std::map<pid_t, posthook_t> posthooks;

class TBatchTask : public TNonCopyable {
public:
    TBatchTask(task_t task, posthook_t post)
        : Task(task), PostHook(post) {};

    TError Run() {

        int ret = fork();
        switch (ret) {
        case -1:
            return TError(EError::Unknown, "");

        case 0:
            /* Child */
            SetDieOnParentExit();
            exit(Task());
            break;

        default:
            /* Parent */
            posthooks[ret] = PostHook;
        }

        return TError::Success();
    }

private:
    task_t Task;
    posthook_t PostHook;
};
