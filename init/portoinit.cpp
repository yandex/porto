#include <vector>
#include <string>
#include <algorithm>

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

static int StartContainers() {
    TFolder f(".");

    std::vector<std::string> files;
    TError error = f.Items(EFileType::Regular, files);
    if (error) {
        TLogger::Log(LOG_ERROR) << "Can't read config directory: " << error << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<std::string> configs;
    std::string suffix = ".conf";
    for (auto &f : files) {
            TLogger::Log() << f << std::endl;
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

    return 0;
}

static void EventLoop() {
}

static int PrepareSystem() {
    // TODO:
    // - setup ttys & console
    // - setup hostname & locale
    // - setup kernel modules
    // - man 5 utmp
    // - mount filesystems
    // - trigger udev
    // - setup network

    return 0;
}

static void CleanupSystem() {
    // TODO: undo everything in PrepareSystem
}

static int StartPorto() {
    return 0;
}

static void StopPorto() {
    // TODO: send SIGINT and wait for exit with timeout
}

static void Restart() {
    // man 2 reboot
}

static void Poweroff() {
    // man 2 reboot
}

// ? SIGPWR/SIGTERM
static bool NeedRestart = false;

int main(int argc, char * const argv[]) {
    int ret;

    TLogger::InitLog("", 0, false);
    TLogger::LogToStd();

    ret = PrepareSystem();
    if (ret)
        return ret;

    ret = StartPorto();
    if (ret)
        return ret;

    ret = StartContainers();
    if (ret)
        return ret;

    EventLoop();

    StopPorto();
    TLogger::CloseLog();

    CleanupSystem();

    if (NeedRestart)
        Restart();
    Poweroff();

    return EXIT_SUCCESS;
}
