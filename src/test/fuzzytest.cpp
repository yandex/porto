#include <vector>
#include <string>
#include <thread>
#include <csignal>

#include "config.hpp"
#include "test.hpp"

namespace test {

static __thread unsigned int seed;

static const std::vector<std::string> names = {
    "a", "a/1", "a/2", "a/3",
    "b", "b/1", "b/2", "b/1/1", "b/1/2", "b/1/3", "b/2/1", "b/2/2",
    "c", "d", "e", "f",
};

static const std::vector<std::pair<std::string, std::vector<std::string>>> propval = {
    { "command", { "sleep 1", "true", "false", "invalid_command" } },
    { "isolate", { "true", "false", } },
    { "memory_limit", { "10485760", "104857600", "0" } },
    { "memory_guarantee", { "10485760", "104857600", "0" } },
    { "recharge_on_pgfault", { "true", "false" } },
    { "cpu_policy", { "rt", "normal", "batch" } },
    { "cpu_limit", { "1", "50", "99", "101" } },
    { "cpu_guarantee", { "1", "50", "99", "101" } },
    { "io_policy", { "normal", "batch", "invalid" } },
    { "respawn", { "true", "false", "-1" } },
    { "max_respawns", { "-1", "0", "5", } },
    { "net", {  "host", "none", "macvlan eth0 eth0" } },
    { "enable_porto", {  "true", "false", } },
};

template<typename T>
const T &GetRandElem(const std::vector<T> &vec) {
    return vec[rand_r(&seed) % vec.size()];
}

const std::string &GetContainer(int n) {
    return names[n % names.size()];
}

static const std::vector<std::function<int(Porto::Connection&, int)>> handlers = {
    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        Say() << "Create " << name << std::endl;
        return api.Create(name);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        Say() << "Destroy " << name << std::endl;
        return api.Destroy(name);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        Say() << "Kill " << name << std::endl;
        return api.Kill(name, 9);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        Say() << "Start " << name << std::endl;
        return api.Start(name);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        Say() << "Stop " << name << std::endl;
        return api.Stop(name);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        Say() << "Pause " << name << std::endl;
        return api.Pause(name);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        Say() << "Resume " << name << std::endl;
        return api.Resume(name);
    },

    [](Porto::Connection &api, int n) {
        std::vector<std::string> list;
        Say() << "List" << std::endl;
        return api.List(list);
    },

    [](Porto::Connection &api, int n) {
        std::vector<Porto::Property> list;
        Say() << "Property list" << std::endl;
        return api.Plist(list);
    },

    [](Porto::Connection &api, int n) {
        std::vector<Porto::Property> list;
        Say() << "Data list" << std::endl;
        return api.Dlist(list);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        std::vector<Porto::Property> list;
        (void)api.Plist(list);

        auto prop = GetRandElem(list);
        std::string val;

        Say() << "Get " << name << " property " << prop.Name << std::endl;
        return api.GetProperty(name, prop.Name, val);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        auto prop = GetRandElem(propval);
        auto key = prop.first;
        auto val = GetRandElem(prop.second);

        Say() << "Set " << name << " property " << key << "=" << val << std::endl;
        return api.SetProperty(name, key, val);
    },

    [](Porto::Connection &api, int n) {
        std::vector<Porto::Property> plist;
        (void)api.Plist(plist);

        std::vector<Porto::Property> dlist;
        (void)api.Dlist(dlist);

        std::vector<std::string> getvar;
        for (auto p : plist)
            getvar.push_back(p.Name);
        for (auto d : dlist)
            getvar.push_back(d.Name);

        std::map<std::string, std::map<std::string, Porto::GetResponse>> result;

        Say() << "Combined get " << std::endl;
        return api.Get(names, getvar, result);
    },

    [](Porto::Connection &api, int n) {
        auto name = GetContainer(n);
        std::vector<Porto::Property> list;
        (void)api.Dlist(list);

        auto data = GetRandElem(list);
        std::string val;

        Say() << "Get " << name << " data " << data.Name << std::endl;
        return api.GetData(name, data.Name, val);
    },

};

static void ThreadMain(int n, int iter) {
    seed = (unsigned int)time(nullptr);
    tid = n + 1;

    Porto::Connection api;

    while (iter--) {
        auto op = GetRandElem(handlers);
        int ret = op(api, n);
        if (ret) {
            int err;
            std::string msg;
            api.GetLastError(err, msg);
            Say() << "ERR " << msg << " (" << err << ")" << std::endl;
        }
    }
    api.Close();
}

int FuzzyTest(int thrnr, int iter) {
    std::vector<std::thread> threads;

    (void)signal(SIGPIPE, SIG_IGN);

    config.Load();
    Porto::Connection api;

    for (auto i = 0; i < thrnr; i++)
        threads.push_back(std::thread(ThreadMain, i, iter));

    for (auto& thr : threads)
        thr.join();

    for (auto name : names)
        api.Destroy(name);

    TestDaemon(api);

    std::cout << "Fuzzy test completed!" << std::endl;

    return 0;
}

}
