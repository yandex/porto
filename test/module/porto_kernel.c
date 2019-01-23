#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxim Samoylov");
MODULE_DESCRIPTION("porto_kernel");
MODULE_VERSION("1.0");

static struct task_struct *d_thread;

static int d_thread_pid = 0;

int threadfn(void *data) {
    while (!kthread_should_stop()) {
        set_current_state(TASK_UNINTERRUPTIBLE);
		schedule();
    }

    return 0;
}

static int __init porto_kernel_init(void) {
    d_thread = kthread_run(threadfn, NULL, "porto_kernel");
    d_thread_pid = d_thread->pid;
    return 0;
}

static void __exit porto_kernel_exit(void) {
    kthread_stop(d_thread);
    wake_up_process(d_thread);
}

module_param(d_thread_pid, int, 0444);
module_init(porto_kernel_init);
module_exit(porto_kernel_exit);
