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

    config().mutable_slavepid()->set_path("/run/portod.pid");
    config().mutable_slavepid()->set_perm(0644);
    config().mutable_slavelog()->set_path("/var/log/portod.log");
    config().mutable_slavelog()->set_perm(0644);

    config().mutable_masterpid()->set_path("/run/portoloop.pid");
    config().mutable_masterpid()->set_perm(0644);
    config().mutable_masterlog()->set_path("/var/log/portoloop.log");
    config().mutable_masterlog()->set_perm(0644);

    config().mutable_rpcsock()->mutable_file()->set_path("/run/portod.socket");
    config().mutable_rpcsock()->mutable_file()->set_perm(0644);
    config().mutable_rpcsock()->set_group("porto");
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
