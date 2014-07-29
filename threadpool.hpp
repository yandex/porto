#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <future>
#include <queue>
#include <condition_variable>

#define USE_CXX11 0

#if !USE_CXX11
#include <pthread.h>
#endif

class TThreadPool {
public:
    TThreadPool(size_t size) : run(true)
    {
#if USE_CXX11
        for (size_t i = 0; i < size; i++)
            workers.emplace_back([this]{ worker_fn(); });
#else
        workers.resize(size);
        for (size_t i = 0; i < size; i++)
            if (pthread_create(&workers[i], nullptr, TThreadPool::worker_fn, this))
                throw "TThreadPool: can't create worker thread";

        if (pthread_mutex_init(&mutex, NULL))
                throw "TThreadPool: can't initialize mutex";
        if (pthread_cond_init(&cond, NULL))
                throw "TThreadPool: can't initialize conditional variable";
#endif
    }

    ~TThreadPool()
    {
#if USE_CXX11
        {
            std::unique_lock<std::mutex> lock(mutex);
            run = false;
        }

        cv.notify_all();

        for (size_t i = 0; i < workers.size(); i++)
            workers[i].join();
#else
        if (pthread_mutex_lock(&mutex))
            throw "TThreadPool: can't lock mutex";
        run = false;
        if (pthread_cond_broadcast(&cond))
            throw "TThreadPool: can't wake up all worker threads";
        if (pthread_mutex_unlock(&mutex))
            throw "TThreadPool: can't unlock mutex";

        for (size_t i = 0; i < workers.size(); i++)
            if (pthread_join(workers[i], nullptr))
                throw "TThreadPool: can't join worker thread";

        (void)pthread_mutex_destroy(&mutex);
        (void)pthread_cond_destroy(&cond);
#endif
    }

    bool Enqueue(std::function<void()> f)
    {
#if USE_CXX11
        std::unique_lock<std::mutex> lock(mutex);

        if (!run)
            return false;

        jobs.push(f);

        cv.notify_one();
#else
        if (pthread_mutex_lock(&mutex))
            throw "TThreadPool: can't lock mutex";

        if (!run) {
            if (pthread_mutex_unlock(&mutex))
                throw "TThreadPool: can't unlock mutex";
            return false;
        }

        jobs.push(f);

        if (pthread_cond_signal(&cond))
            throw "TThreadPool: can't wake worker thread";

        if (pthread_mutex_unlock(&mutex))
            throw "TThreadPool: can't unlock mutex";
#endif

        return true;
    }
private:
    bool run;
    std::queue< std::function<void()> > jobs;
#if USE_CXX11
    std::vector< std::thread > workers;
    std::mutex mutex;
    std::condition_variable cv;
#else
    std::vector<pthread_t> workers;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif

#if USE_CXX11
    void worker_fn()
    {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);

            while (run && jobs.empty()) {
                cv.wait(lock);
            }

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
#else
    static void *worker_fn(void *arg)
    {
        TThreadPool *pool = static_cast<TThreadPool *>(arg);

        while (true) {
            if (pthread_mutex_lock(&pool->mutex))
                throw "TThreadPool: can't lock mutex";

            while (pool->run && pool->jobs.empty()) {
                if (pthread_cond_wait(&pool->cond, &pool->mutex))
                    throw "TThreadPool: can't wait on conditional variable";
            }

            if (!pool->run) {
                if (pthread_mutex_unlock(&pool->mutex))
                    throw "TThreadPool: can't unlock mutex";
                return nullptr;
            }

            auto f(pool->jobs.front());
            pool->jobs.pop();
            if (pthread_mutex_unlock(&pool->mutex))
                throw "TThreadPool: can't unlock mutex";
            f();
        }
    }
#endif
};

#endif
