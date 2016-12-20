#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <thread>
#include <cmath>

extern "C" {
    #include <stdio.h>
    #include <signal.h>
    #include <time.h>
    #include <unistd.h>
    #include <openssl/evp.h>
}

int inline get_ts(const clockid_t clkid, uint64_t &ts) {
    struct timespec cur;
    if (clock_gettime(clkid, &cur) < 0)
        return -1;

    ts = cur.tv_sec * 1000000000 + cur.tv_nsec;

    return 0;
}

struct Worker {
    bool Running;
    std::shared_ptr<std::thread> Thread = nullptr;

    void fn() {
        auto buffer = new uint8_t[1024];
        EVP_MD_CTX *ctx = EVP_MD_CTX_create();
        EVP_MD *md = const_cast<EVP_MD *>(EVP_sha1());

        for (int i = 0; i < 1024; i++)
            buffer[i] = (uint8_t)(i % 256);

        EVP_DigestInit(ctx, md);

        while (Running)
            EVP_DigestUpdate(ctx, buffer, 1024);

        EVP_DigestFinal(ctx, buffer, NULL);
        EVP_MD_CTX_destroy(ctx);
    }

    int Start() {
        if (Thread)
            return -1;

        Running = true;
        Thread = std::make_shared<std::thread>(&Worker::fn, this);

        return 0;
    }

    int Wait() {
        if (!Thread)
            return -1;

        Thread->join();
        Thread = nullptr;

        return 0;
    }
};

std::string help = ": worker_num time [guarantee_ratio limit_ratio [check_interval]]";

int main(int argc, char **argv) {
    std::vector<std::shared_ptr<Worker>> threads;
    double ratio = 0.0;
    bool fail = false;

    if (argc < 3) {
        std::cerr << argv[0] << help << std::endl;
        return -1;
    }

    int tnum = std::stoi(std::string(argv[1]));
    uint64_t runtime = std::stoul(std::string(argv[2])); /* ms */

    double guarantee = 0.0;
    double limit = 0.0;
    uint64_t interval = 1000;

    if (argc > 3)
       guarantee = std::stof(std::string(argv[3]));

    if (argc > 4)
       limit = std::stof(std::string(argv[4]));

    if (!limit)
        limit = 1000000.0;

    if (argc > 5)
        interval = std::stoul(std::string(argv[5])); /* ms */

    if (argc > 6) {
        std::cerr << argv[0] << help << std::endl;
        return -1;
    }

    runtime *= 1000000; /* ms */
    interval *= 1000;

    for (int i = 0; i < tnum; i++)
        threads.push_back(std::make_shared<Worker>());

    uint64_t start_ts = 0llu;
    get_ts(CLOCK_MONOTONIC, start_ts);

    for (auto &i : threads)
        i->Start();

    uint64_t last_ts, last_ts_cpu;

    if (!get_ts(CLOCK_MONOTONIC, last_ts) &&
        !get_ts(CLOCK_PROCESS_CPUTIME_ID, last_ts_cpu)) {
        uint64_t ts = 0llu, ts_cpu = 0llu;
        uint64_t finish = last_ts + runtime;

        do {
            usleep((finish - ts) / 1000 > interval ? interval : (finish - ts) / 1000);

            if (get_ts(CLOCK_PROCESS_CPUTIME_ID, ts_cpu))
                return -1;

            if (get_ts(CLOCK_MONOTONIC, ts))
                return -1;

            ratio = double (ts_cpu - last_ts_cpu) / (ts - last_ts);

            last_ts = ts;
            last_ts_cpu = ts_cpu;

            if (ratio < guarantee || ratio > limit) {
                fail = true;
                break;
            }
        } while (ts < finish);
    }

    for (auto &i : threads) {
        i->Running = false;
        i->Wait();
    }

    uint64_t end_ts;
    get_ts(CLOCK_MONOTONIC, end_ts);

    start_ts /= 1000000;
    end_ts /= 1000000;

    int err = 0;

    if (fail) {
        std::cout << "Run failed, ratio: " << ratio;
        err = -1;
    } else {
        std::cout << ratio;
    }

    //std::cout << " start: " << start_ts << " end: " << end_ts;
    std::cout << std::endl;

    return err;
}
