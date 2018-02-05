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

    std::string version;
    if (!GetSysctl("kernel.osrelease", version))
        config().set_linux_version(version);

    config().mutable_log()->set_verbose(false);
    config().mutable_log()->set_debug(false);

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
    config().mutable_daemon()->set_helpers_dirty_limit(256ull << 20);
    config().mutable_daemon()->set_workers(32);
    config().mutable_daemon()->set_max_msg_len(32 * 1024 * 1024);
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
    config().mutable_container()->set_empty_wait_timeout_ms(5000);
    config().mutable_container()->set_enable_cpu_reserve(true);
    config().mutable_container()->set_rt_priority(10);
    config().mutable_container()->set_rt_nice(-20);
    config().mutable_container()->set_high_nice(-10);
    config().mutable_container()->set_enable_tracefs(true);
    config().mutable_container()->set_devpts_max(256);
    config().mutable_container()->set_dev_size(32 << 20);
    config().mutable_container()->set_enable_hugetlb(true);
    config().mutable_container()->set_min_memory_limit(1ull << 20); /* 1Mb */

    config().mutable_container()->set_dead_memory_soft_limit(1 << 20); /* 1Mb */
    config().mutable_container()->set_pressurize_on_death(false);

    config().mutable_container()->set_default_ulimit("core: 0 unlimited; memlock: 8M unlimited; nofile: 8K 1M");
    config().mutable_container()->set_default_thread_limit(10000);

    config().mutable_container()->set_cpu_period(100000000);    /* 100ms */
    config().mutable_container()->set_propagate_cpu_guarantee(true);

    config().mutable_container()->set_enable_systemd(true);
    config().mutable_container()->set_detect_systemd(true);

    config().mutable_volumes()->set_enable_quota(true);

    if (CompareVersions(config().linux_version(), "4.4") >= 0)
        config().mutable_volumes()->set_direct_io_loop(true);

    config().mutable_volumes()->set_max_total(3000);
    config().mutable_volumes()->set_place_load_limit("default: 2; /ssd: 4");
    config().mutable_volumes()->set_squashfs_compression("gzip");

    config().mutable_volumes()->set_owner_container_migration_hack(true); /* FIXME kill it */

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
    config().mutable_network()->set_proxy_ndp_watchdog_ms(60000);
    config().mutable_network()->set_watchdog_ms(5000);

    config().mutable_network()->set_cache_statistics_ms(1000);

    config().mutable_network()->set_l3_migration_hack(true); /* FIXME kill it */

    config().mutable_network()->set_ipip6_encap_limit(4);
    config().mutable_network()->set_ipip6_ttl(64);

    config().mutable_network()->set_enable_ip6tnl0(true);
    config().mutable_network()->set_enable_iproute(false);

    config().mutable_core()->set_enable(false);
    config().mutable_core()->set_timeout_s(600); /* 10min */
    config().mutable_core()->set_space_limit_mb(102400); /* 100Gb */
    config().mutable_core()->set_slot_space_limit_mb(10240); /* 10Gb */
    config().mutable_core()->set_sync_size(4ull << 20); /* 4Mb */

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

void TConfig::Load() {
    LoadDefaults();

    if (!LoadFile("/etc/portod.conf"))
        LoadFile("/etc/default/portod.conf");

    Debug |= config().log().debug();
    Verbose |= Debug | config().log().verbose();
}

cfg::TCfg &TConfig::operator()() {
    return Cfg;
}
