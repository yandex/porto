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

    TError Run(TContext &context);

private:
    task_t Task;
    posthook_t PostHook;

    TError RunSync(TContext &context);
    TError RunAsync(TContext &context);
};
