#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <future>
#include <queue>
#include <condition_variable>

class TThreadPool {
public:
    TThreadPool(size_t size) : run(true)
    {
        for (size_t i = 0; i < size; i++)
            workers.emplace_back([this]{ worker_fn(); });
    }

    ~TThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(mutex);
            run = false;
        }

        condition.notify_all();

        for (size_t i = 0; i < workers.size(); i++)
            workers[i].join();
    }

    bool Enqueue(std::function<void()> f)
    {
        std::unique_lock<std::mutex> lock(mutex);

        if (!run)
            return false;

        jobs.push(f);

        condition.notify_one();

        return true;
    }
private:
    std::vector< std::thread > workers;
    std::queue< std::function<void()> > jobs;

    std::mutex mutex;
    std::condition_variable condition;
    bool run;

    void worker_fn()
    {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);

            while (run && jobs.empty())
                condition.wait(lock);

            if (!run) {
                lock.unlock();
                return;
            }

            auto f(jobs.front());
            jobs.pop();
            lock.unlock();
            f();
        }
    }
};

#endif
