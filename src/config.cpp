#include "config.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"
#include "util/namespace.hpp"

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <algorithm>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

class TProtobufLogger : public google::protobuf::io::ErrorCollector {
public:
    std::string Path;
    TProtobufLogger(const std::string &path) : Path(path) {}
    ~TProtobufLogger() {}

    void AddError(int line, int column, const std::string& message) {
        L_WRN("Config {} at line {} column {} {}", Path, line + 1, column + 1, message);
    }

    void AddWarning(int line, int column, const std::string& message) {
        L_WRN("Config {} at line {} column {} {}", Path, line + 1, column + 1, message);
    }
};

static cfg::TConfig Config;

cfg::TConfig &config() {
    return Config;
}

static void NetSysctl(const std::string &key, const std::string &val)
{
    auto sysctl = config().mutable_container()->add_net_sysctl();
    sysctl->set_key(key);
    sysctl->set_val(val);
}

static void DefaultConfig() {
    std::string version;
    if (!GetSysctl("kernel.osrelease", version))
        config().set_linux_version(version);

    config().mutable_log()->set_verbose(false);
    config().mutable_log()->set_debug(false);

    config().set_keyvalue_limit(1 << 20);
    config().set_keyvalue_size(32 << 20);

    config().mutable_daemon()->set_rw_threads(20);
    config().mutable_daemon()->set_ro_threads(10);
    config().mutable_daemon()->set_io_threads(5);
    config().mutable_daemon()->set_vl_threads(5);
    config().mutable_daemon()->set_tar_path("tar");
    config().mutable_daemon()->set_request_handling_delay_ms(0);

    config().mutable_daemon()->set_max_clients(1000);
    config().mutable_daemon()->set_max_clients_in_container(500);
    config().mutable_daemon()->set_cgroup_remove_timeout_s(300);
    config().mutable_daemon()->set_freezer_wait_timeout_s(5 * 60);

    config().mutable_daemon()->set_memory_guarantee_reserve(2ull << 30); /* 2Gb */

    config().mutable_daemon()->set_log_rotate_ms(1000);
    config().mutable_daemon()->set_memory_limit(1ull << 30);
    config().mutable_daemon()->set_helpers_memory_limit(1ull << 30);
    config().mutable_daemon()->set_helpers_dirty_limit(256ull << 20);
    config().mutable_daemon()->set_max_msg_len(32 * 1024 * 1024);
    config().mutable_daemon()->set_portod_stop_timeout(300);
    config().mutable_daemon()->set_portod_start_timeout(300);
    config().mutable_daemon()->set_portod_shutdown_timeout(60);
    config().mutable_daemon()->set_merge_memory_blkio_controllers(false);
    config().mutable_daemon()->set_client_idle_timeout(60);
    config().mutable_daemon()->set_debug_hung_tasks_count(10);

    config().mutable_container()->set_default_aging_time_s(60 * 60 * 24);
    config().mutable_container()->set_respawn_delay_ms(1000);

    config().mutable_container()->set_stdout_limit(8 << 20); /* 8Mb */
    config().mutable_container()->set_stdout_limit_max(1 << 30); /* 1Gb */
    config().mutable_container()->set_std_stream_read_limit(16 << 20); /* 16Mb */

    config().mutable_container()->set_kill_timeout_ms(1000);
    config().mutable_container()->set_start_timeout_ms(300 * 1000);
    // wait 30 seconds for container process to exit after SIGKILL
    config().mutable_container()->set_stop_timeout_ms(30 * 1000);
    config().mutable_container()->set_max_total(3000);
    config().mutable_container()->set_empty_wait_timeout_ms(5000);
    config().mutable_container()->set_enable_cpu_reserve(true);
    config().mutable_container()->set_rt_priority(0);
    config().mutable_container()->set_rt_nice(-20);
    config().mutable_container()->set_high_nice(-10);
    config().mutable_container()->set_enable_tracefs(true);
    config().mutable_container()->set_devpts_max(256);
    config().mutable_container()->set_dev_size(32 << 20);
    config().mutable_container()->set_enable_hugetlb(true);
    config().mutable_container()->set_enable_blkio(true);
    config().mutable_container()->set_ptrace_on_start(false);

    if (CompareVersions(config().linux_version(), "4.19") >= 0)
        config().mutable_container()->set_enable_cgroup2(true);

    config().mutable_container()->set_use_os_mode_cgroupns(false);
    config().mutable_container()->set_enable_rw_cgroupfs(false);

    config().mutable_container()->set_enable_docker_mode(false);

    config().mutable_container()->set_min_memory_limit(1ull << 20); /* 1Mb */

    // config().mutable_container()->set_memory_limit_margin(2ull << 30); /* 2Gb */

    // config().mutable_container()->set_anon_limit_margin(16ull << 20); /* 16Mb */

    config().mutable_container()->set_memlock_minimal(8ull << 20); /* 8Mb */
    config().mutable_container()->set_memlock_margin(16ull << 20); /* 16Mb */

    config().mutable_container()->set_dead_memory_soft_limit(1 << 20); /* 1Mb */
    config().mutable_container()->set_pressurize_on_death(false);

    config().mutable_container()->set_default_ulimit("core: 0 unlimited; nofile: 8K 1M");
    config().mutable_container()->set_default_thread_limit(10000);

    config().mutable_container()->set_cpu_period(100000000);    /* 100ms */
    config().mutable_container()->set_propagate_cpu_guarantee(true);

    config().mutable_container()->set_enable_systemd(true);
    config().mutable_container()->set_detect_systemd(true);

    config().mutable_volumes()->set_enable_quota(true);
    config().mutable_volumes()->set_keep_project_quota_id(true);
    config().mutable_volumes()->set_insecure_user_paths(true);

    if (CompareVersions(config().linux_version(), "4.4") >= 0)
        config().mutable_volumes()->set_direct_io_loop(true);

    config().mutable_volumes()->set_max_total(3000);
    config().mutable_volumes()->set_place_load_limit("default: 2; /ssd: 4");
    config().mutable_volumes()->set_squashfs_compression("gzip");

    config().mutable_volumes()->set_aux_default_places("");

    config().mutable_volumes()->set_fs_stat_update_interval_ms(60000);

    config().mutable_network()->set_enable_host_net_classes(false);

    config().mutable_network()->set_device_qdisc("default: htb");

    config().mutable_network()->set_default_rate("default: 1250000");  /* 10Mbit */
    config().mutable_network()->set_default_qdisc("default: fq_codel");
    config().mutable_network()->set_default_qdisc_limit("default: 10240");

    config().mutable_network()->set_container_rate("default: 125000");  /* 1Mbit */
    config().mutable_network()->set_container_qdisc("default: fq_codel");
    config().mutable_network()->set_container_qdisc_limit("default: 10240");

    config().mutable_network()->set_autoconf_timeout_s(120);
    config().mutable_network()->set_proxy_ndp(true);
    config().mutable_network()->set_proxy_ndp_max_range(16);
    config().mutable_network()->set_proxy_ndp_watchdog_ms(60000);
    config().mutable_network()->set_watchdog_ms(1000);
    config().mutable_network()->set_resolv_conf_watchdog_ms(5000);
    config().mutable_network()->set_sock_diag_update_interval_ms(5000);
    config().mutable_network()->set_sock_diag_max_fds(100000);

    config().mutable_network()->set_managed_ip6tnl(false);
    config().mutable_network()->set_managed_vlan(false);
    config().mutable_network()->set_enforce_unmanaged_defaults(false);

    config().mutable_network()->set_cache_statistics_ms(1000);

    config().mutable_network()->set_l3_migration_hack(true); /* FIXME kill it */

    config().mutable_network()->set_ipip6_encap_limit(4);
    config().mutable_network()->set_ipip6_ttl(64);
    config().mutable_network()->set_ipip6_tx_queues(8);

    config().mutable_network()->set_enable_ip6tnl0(true);
    config().mutable_network()->set_enable_iproute(false);
    config().mutable_network()->set_enable_netcls_priority(true);
    config().mutable_network()->set_l3_default_mtu(9000);
    config().mutable_network()->set_default_uplink_speed_gb(10);

    config().mutable_network()->set_l3stat_watchdog_ms(25);
    config().mutable_network()->set_l3stat_watchdog_lost_ms(50);

    config().mutable_core()->set_enable(false);
    config().mutable_core()->set_timeout_s(600); /* 10min */
    config().mutable_core()->set_space_limit_mb(102400); /* 100Gb */
    config().mutable_core()->set_slot_space_limit_mb(10240); /* 10Gb */
    config().mutable_core()->set_sync_size(4ull << 20); /* 4Mb */

    NetSysctl("net.ipv6.conf.all.accept_dad", "0");
    NetSysctl("net.ipv6.conf.default.accept_dad", "0");
    NetSysctl("net.ipv6.auto_flowlabels", "0");
}

static TError ReadConfig(const TPath &path, bool silent) {
    TError error;
    TFile file;

    error = file.OpenRead(path);
    if (error) {
        if (!silent && error.Errno != ENOENT)
            L_WRN("Cannot read config {} {}", path, error);
        return error;
    }

    google::protobuf::io::FileInputStream stream(file.Fd);
    google::protobuf::TextFormat::Parser parser;
    TProtobufLogger logger(path.ToString());

    if (!silent) {
        L_SYS("Read config {}", path);
        parser.RecordErrorsTo(&logger);
    }

    bool ok = parser.Merge(&stream, &Config);
    if (!ok && !silent)
        L_WRN("Cannot parse config {} the rest is skipped", path);

    return OK;
}

void ReadConfigs(bool silent) {
    Config.Clear();
    DefaultConfig();

    if (ReadConfig("/etc/portod.conf", silent))
        ReadConfig("/etc/default/portod.conf", silent); /* FIXME remove */

    TPath config_dir = "/etc/portod.conf.d";
    std::vector<std::string> config_names;
    config_dir.ReadDirectory(config_names);
    std::sort(config_names.begin(), config_names.end());
    for (auto &name: config_names) {
        if (StringEndsWith(name, ".conf"))
            ReadConfig(config_dir / name, silent);
    }

    Debug |= config().log().debug();
    Verbose |= Debug | config().log().verbose();
}

TError ValidateConfig() {
#ifndef __x86_64__
    if (config().container().ptrace_on_start())
        return TError(EError::InvalidMethod, "ptrace_on_start function not implemented");
#endif

#if TCA_FQ_CODEL_MAX <= 8
    if (config().network().has_fq_codel_memory_limit() &&
        config().network().fq_codel_memory_limit() > 0)
        return TError(EError::InvalidValue, "fq_codel memory limit not supported");
#endif

    return OK;
}
