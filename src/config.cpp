#include <iostream>

#include "config.hpp"
#include "protobuf.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"
#include "util/namespace.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

TConfig config;

static void NetSysctl(const std::string &key, const std::string &val)
{
    auto sysctl = config().mutable_container()->add_net_sysctl();
    sysctl->set_key(key);
    sysctl->set_val(val);
}

void TConfig::LoadDefaults() {
    config().Clear();

    config().mutable_log()->set_verbose(false);

    config().set_keyvalue_limit(1 << 20);
    config().set_keyvalue_size(32 << 20);

    config().mutable_daemon()->set_max_clients(1000);
    config().mutable_daemon()->set_max_clients_in_container(500);
    config().mutable_daemon()->set_cgroup_remove_timeout_s(300);
    config().mutable_daemon()->set_freezer_wait_timeout_s(5 * 60);
    config().mutable_daemon()->set_memory_guarantee_reserve(2 * 1024 * 1024 * 1024UL);
    config().mutable_daemon()->set_log_rotate_ms(1000);
    config().mutable_daemon()->set_memory_limit(1ull << 30);
    config().mutable_daemon()->set_helpers_memory_limit(1ull << 30);
    config().mutable_daemon()->set_workers(32);
    config().mutable_daemon()->set_max_msg_len(32 * 1024 * 1024);
    config().mutable_daemon()->set_event_workers(1);
    config().mutable_daemon()->set_portod_stop_timeout(30);
    config().mutable_daemon()->set_portod_start_timeout(60);
    config().mutable_daemon()->set_merge_memory_blkio_controllers(false);
    config().mutable_daemon()->set_client_idle_timeout(60);

    config().mutable_container()->set_default_aging_time_s(60 * 60 * 24);
    config().mutable_container()->set_respawn_delay_ms(1000);

    config().mutable_container()->set_stdout_limit(8 << 20); /* 8Mb */
    config().mutable_container()->set_stdout_limit_max(1 << 30); /* 1Gb */

    config().mutable_container()->set_private_max(1024);
    config().mutable_container()->set_kill_timeout_ms(1000);
    config().mutable_container()->set_start_timeout_ms(300 * 1000);
    // wait 30 seconds for container process to exit after SIGKILL
    config().mutable_container()->set_stop_timeout_ms(30 * 1000);
    config().mutable_container()->set_max_total(3000);
    config().mutable_container()->set_batch_io_weight(10);
    config().mutable_container()->set_normal_io_weight(500);
    config().mutable_container()->set_empty_wait_timeout_ms(5000);
    config().mutable_container()->set_enable_smart(false);
    config().mutable_container()->set_enable_cpu_reserve(true);
    config().mutable_container()->set_rt_priority(10);
    config().mutable_container()->set_rt_nice(-20);
    config().mutable_container()->set_high_nice(-10);
    config().mutable_container()->set_default_porto_namespace(false);
    config().mutable_container()->set_enable_tracefs(true);
    config().mutable_container()->set_devpts_max(256);
    config().mutable_container()->set_dev_size(32 << 20);
    config().mutable_container()->set_all_controllers(false);
    config().mutable_container()->set_enable_hugetlb(true);
    config().mutable_container()->set_min_memory_limit(1ull << 20); /* 1Mb */

    config().mutable_container()->set_default_ulimit("core: 0 unlimited; memlock: 8M unlimited; nofile: 8K 1M");

    config().mutable_volumes()->set_enable_quota(true);
    config().mutable_volumes()->set_max_total(3000);

    config().mutable_network()->set_device_qdisc("default: hfsc");
    config().mutable_network()->set_default_rate("default: 125000");    /* 1Mbit */
    config().mutable_network()->set_porto_rate("default: 125000");      /* 1Mbit */
    config().mutable_network()->set_container_rate("default: 125000");  /* 1Mbit */

    config().mutable_network()->set_default_qdisc("default: sfq");
    config().mutable_network()->set_default_qdisc_limit("default: 10000");

    config().mutable_network()->set_container_qdisc("default: pfifo");
    config().mutable_network()->set_container_qdisc_limit("default: 1000");

    config().mutable_network()->set_autoconf_timeout_s(120);
    config().mutable_network()->set_proxy_ndp(true);
    config().mutable_network()->set_watchdog_ms(60000);

    config().mutable_core()->set_enable(false);
    config().mutable_core()->set_timeout_s(600); /* 10min */
    config().mutable_core()->set_space_limit_mb(1024); /* 1Gb */

    NetSysctl("net.ipv6.conf.all.accept_dad", "0");
    NetSysctl("net.ipv6.conf.default.accept_dad", "0");
}

bool TConfig::LoadFile(const std::string &path) {
    TFile file;

    if (file.OpenRead(path))
        return false;

    google::protobuf::io::FileInputStream pist(file.Fd);

    if (!google::protobuf::TextFormat::Merge(&pist, &Cfg) ||
        !Cfg.IsInitialized()) {
        return false;
    }

    return true;
}

static void InitIpcSysctl() {
    for (const auto &key: IpcSysctls) {
        bool set = false;
        for (const auto &it: config().container().ipc_sysctl())
            set |= it.key() == key;
        std::string val;
        /* load default ipc sysctl from host config */
        if (!set && !GetSysctl(key, val)) {
            auto sysctl = config().mutable_container()->add_ipc_sysctl();
            sysctl->set_key(key);
            sysctl->set_val(val);
        }
    }
}

void TConfig::Load() {
    LoadDefaults();

    if (!LoadFile("/etc/portod.conf"))
        LoadFile("/etc/default/portod.conf");

    Verbose |= config().log().verbose();

    InitCred();
    InitCapabilities();
    InitIpcSysctl();
}

int TConfig::Test(const std::string &path) {
    TFile file;

    if (access(path.c_str(), F_OK)) {
        std::cerr << "Config " << path << " doesn't exist" << std::endl;
        return EXIT_FAILURE;
    }

    if (file.OpenRead(path)) {
        std::cerr << "Can't open " << path << std::endl;
        return EXIT_FAILURE;
    }

    google::protobuf::io::FileInputStream pist(file.Fd);

    cfg::TCfg cfg;
    if (!google::protobuf::TextFormat::Merge(&pist, &cfg))
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

cfg::TCfg &TConfig::operator()() {
    return Cfg;
}
