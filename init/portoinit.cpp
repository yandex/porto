#include <vector>
#include <string>

#include "init.pb.h"

#include "util/protobuf.hpp"
#include "util/log.hpp"
#include "util/folder.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

static bool ExecConfig(const init::TConfig &cfg) {
    // TODO
    TLogger::Log() << cfg.DebugString();

    return true;
}

static bool LoadConfig(const std::string &path, init::TConfig &cfg) {
    int fd = open(path.c_str(), O_RDONLY);
    google::protobuf::io::FileInputStream pist(fd);

    cfg.Clear();
    if (!google::protobuf::TextFormat::Parse(&pist, &cfg) ||
        !cfg.IsInitialized()) {
        close(fd);
        return false;
    }

    close(fd);
    return true;
}


int main(int argc, char * const argv[]) {
    TFolder f(".");

    TLogger::InitLog("", 0);
    TLogger::LogToStd();

    std::vector<std::string> files;
    TError error = f.Items(TFile::Regular, files);
    if (error) {
        TLogger::LogError(error, "Can't read config directory");
        return EXIT_FAILURE;
    }

    std::vector<std::string> configs;
    std::string suffix = ".conf";
    for (auto &f : files) {
        if (suffix.length() >= f.length())
            continue;

        if (f.compare(f.length() - suffix.length(), suffix.length(), suffix) != 0)
            continue;

        configs.push_back(f);
    }

    std::sort(configs.begin(), configs.end());

    for (auto &path : configs) {
        TLogger::Log() << "Loading " << path << std::endl;

        init::TConfig cfg;
        if (!LoadConfig(path, cfg)) {
            TLogger::Log() << "Failed" << std::endl;
            continue;
        }

        ExecConfig(cfg);
    }

    TLogger::CloseLog();

    return EXIT_SUCCESS;
}
