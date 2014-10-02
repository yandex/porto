#include <iostream>

#include "config.hpp"
#include "util/protobuf.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
}

using std::string;

TConfig config;

void TConfig::LoadDefaults() {
    config().mutable_network()->set_enabled(true);
    config().mutable_network()->set_debug(false);
    config().mutable_network()->set_device("");

    config().mutable_slave_pid()->set_path("/run/portod.pid");
    config().mutable_slave_pid()->set_perm(0644);
    config().mutable_slave_log()->set_path("/var/log/portod.log");
    config().mutable_slave_log()->set_perm(0644);

    config().mutable_master_pid()->set_path("/run/portoloop.pid");
    config().mutable_master_pid()->set_perm(0644);
    config().mutable_master_log()->set_path("/var/log/portoloop.log");
    config().mutable_master_log()->set_perm(0644);

    config().mutable_rpc_sock()->mutable_file()->set_path("/run/portod.socket");
    config().mutable_rpc_sock()->mutable_file()->set_perm(0644);
    config().mutable_rpc_sock()->set_group("porto");

    config().mutable_log()->set_verbose(false);

    config().mutable_keyval()->mutable_file()->set_path("/run/porto/kvs");
    config().mutable_keyval()->mutable_file()->set_perm(0644);
    config().mutable_keyval()->set_size("size=32m");

    config().mutable_daemon()->set_max_clients(16);
    config().mutable_daemon()->set_poll_timeout_ms(10 * 1000);
    config().mutable_daemon()->set_heartbead_delay_ms(5 * 1000);
    config().mutable_daemon()->set_watchdog_max_fails(5);
    config().mutable_daemon()->set_watchdog_delay_s(5);
    config().mutable_daemon()->set_wait_timeout_s(10);
    config().mutable_daemon()->set_read_timeout_s(5);
    config().mutable_daemon()->set_cgroup_remove_timeout_s(1);
    config().mutable_daemon()->set_freezer_wait_timeout_s(1);
    config().mutable_daemon()->set_memory_guarantee_reserve(2 * 1024 * 1024 * 1024UL);
    config().mutable_daemon()->mutable_pidmap()->set_path("/tmp/portod.pidmap");

    config().mutable_container()->set_max_log_size(10 * 1024 * 1024);
    config().mutable_container()->set_tmp_dir("/place/porto");
    config().mutable_container()->set_aging_time_ms(60 * 60 * 24 * 7 * 1000);
    config().mutable_container()->set_respawn_delay_ms(1000);
    config().mutable_container()->set_stdout_read_bytes(8 * 1024 * 1024);
}

bool TConfig::LoadFile(const std::string &path, bool silent) {
    TScopedFd fd = open(path.c_str(), O_RDONLY);
    if (fd.GetFd() < 0)
        return false;

    google::protobuf::io::FileInputStream pist(fd.GetFd());

    if (!google::protobuf::TextFormat::Merge(&pist, &Cfg) ||
        !Cfg.IsInitialized()) {
        return false;
    }

    if (!silent)
        std::cerr << "Using config " << path << std::endl;

    return true;
}

void TConfig::Load(bool silent) {
    LoadDefaults();

    if (LoadFile("/etc/portod.conf", silent))
        return;
    if (LoadFile("/etc/default/portod.conf", silent))
        return;

    if (!silent)
        std::cerr << "Using default config" << std::endl;
}

cfg::TCfg &TConfig::operator()() {
    return Cfg;
}
