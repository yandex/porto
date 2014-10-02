#include "config.hpp"
#include "util/log.hpp"
#include "util/protobuf.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
}

using std::string;

TConfig config;

void TConfig::LoadDefaults() {
    config().mutable_network()->set_enabled(true);

    TLogger::Log() << "Load default config: " << config().DebugString() << std::endl;
}

bool TConfig::LoadFile(const std::string &path) {
    TScopedFd fd = open(path.c_str(), O_RDONLY);
    if (fd.GetFd() < 0)
        return false;

    google::protobuf::io::FileInputStream pist(fd.GetFd());

    Cfg.Clear();
    if (!google::protobuf::TextFormat::Merge(&pist, &Cfg) ||
        !Cfg.IsInitialized()) {
        return false;
    }

    TLogger::Log() << "Using config " << path << std::endl;

    return true;
}
void TConfig::Load() {
    LoadDefaults();

    if (LoadFile("/etc/portod.conf"))
        return;
    LoadFile("/etc/default/portod.conf");
}

cfg::TCfg &TConfig::operator()() {
    return Cfg;
}
