#include <iostream>

#include "config.hpp"
#include "protobuf.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

TConfig config;

void TConfig::LoadDefaults() {
    config().Clear();

    config().mutable_slave_log()->set_path("/var/log/portod.log");
    config().mutable_slave_log()->set_perm(0644);

    config().mutable_master_log()->set_path("/var/log/portoloop.log");
    config().mutable_master_log()->set_perm(0644);

    config().mutable_log()->set_verbose(false);

    config().mutable_keyval()->mutable_file()->set_path("/run/porto/kvs");

    config().set_keyvalue_limit(1 << 20);
    config().set_keyvalue_size(32 << 20);

    config().mutable_daemon()->set_max_clients(1000);
    config().mutable_daemon()->set_cgroup_remove_timeout_s(5);
    config().mutable_daemon()->set_freezer_wait_timeout_s(5 * 60);
    config().mutable_daemon()->set_memory_guarantee_reserve(2 * 1024 * 1024 * 1024UL);
    config().mutable_daemon()->set_rotate_logs_timeout_s(60);
    config().mutable_daemon()->set_sysfs_root("/sys/fs/cgroup");
    config().mutable_daemon()->set_memory_limit(1ull << 30);
    config().mutable_daemon()->set_helpers_memory_limit(1ull << 30);
    config().mutable_daemon()->set_workers(32);
    config().mutable_daemon()->set_max_msg_len(32 * 1024 * 1024);
    config().mutable_daemon()->set_event_workers(1);
    config().mutable_daemon()->set_portod_stop_timeout(30);
    config().mutable_daemon()->set_portod_start_timeout(60);

    config().mutable_container()->set_tmp_dir("/place/porto");
    config().mutable_container()->set_chroot_porto_dir("porto");
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
    config().mutable_container()->set_enable_smart(true);
    config().mutable_container()->set_default_porto_namespace(false);
    config().mutable_container()->set_enable_tracefs(true);
    config().mutable_container()->set_devpts_max(256);
    config().mutable_container()->set_dev_size(32 << 20);
    config().mutable_container()->set_all_controllers(false);
    config().mutable_container()->set_legacy_porto(false);
    config().mutable_container()->set_enable_hugetlb(true);

    config().mutable_volumes()->mutable_keyval()->mutable_file()->set_path("/run/porto/pkvs");

    config().mutable_volumes()->set_default_place("/place");
    config().mutable_volumes()->set_volume_dir("porto_volumes");
    config().mutable_volumes()->set_layers_dir("porto_layers");
    config().mutable_volumes()->set_enable_quota(true);
    config().mutable_volumes()->set_max_total(3000);

    config().mutable_network()->set_device_qdisc("default: hfsc");
    config().mutable_network()->set_default_rate("default: 125000");    /* 1Mbit */
    config().mutable_network()->set_porto_rate("default: 125000");      /* 1Mbit */
    config().mutable_network()->set_container_rate("default: 125000");  /* 1Mbit */
    config().mutable_network()->set_default_qdisc("default: sfq");
    config().mutable_network()->set_default_qdisc_limit("default: 10000");
    config().mutable_network()->set_container_qdisc("default: bfifo");

    config().mutable_network()->set_autoconf_timeout_s(120);
    config().mutable_network()->set_proxy_ndp(true);

    // FIXME set to true and deprecate this option
    config().mutable_privileges()->set_enforce_bind_permissions(false);
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

TError TConfig::Load() {
    TError error;

    LoadDefaults();

    if (!LoadFile("/etc/portod.conf"))
        LoadFile("/etc/default/portod.conf");

    Verbose |= config().log().verbose();

    error = InitCred();
    if (error) {
        L_ERR() << "Failed initialize credentials" << std::endl;
        return error;
    }

    InitCapabilities();

    return error;
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
