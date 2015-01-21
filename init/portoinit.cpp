#include <vector>
#include <string>
#include <map>
#include <algorithm>

#include "init.pb.h"

#include "util/protobuf.hpp"
#include "util/log.hpp"
#include "util/folder.hpp"
#include "util/file.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

static std::string CONFIG_DIR = "/etc/portoinit";
static std::string extension = ".conf";

static std::map<std::string, std::map<std::string, std::string>> containers;

static bool ExecConfig(const init::TConfig &cfg) {
    return true;
}

static bool GetContainerName(std::string &f, std::string &containerNameTmp) {
    if (*f.begin() == '[' and *f.rbegin() == ']' and f.size() > 4) {
        containerNameTmp = f.substr(1, f.size()-2);
        return true;
    }
    return false;
}

static bool GetContainerProperty(std::string &f, std::string &parameterName, std::string &parameterValue) {
    std::string::size_type n;
    n = f.find('=');
    if (n != std::string::npos) {
        parameterName = f.substr(0, n);
        parameterValue = f.substr(n + 1, f.size());
        return true;
    }
    return false;
}

static bool LoadConfig(const std::string &path, init::TConfig &cfg, std::map<std::string, std::map<std::string, std::string>> &containers) {
    std::vector<std::string> config;
    std::string containerNameTmp, containerName, parameterName, parameterValue;
    std::map<std::string, std::map<std::string, std::string>> container;

    TFile f(path);
    f.AsLines(config);
    for(auto &f: config){
        if (f == "") {
            continue;
        }
        if (GetContainerProperty(f, parameterName, parameterValue)) {
            if(containerName == "") {
                TLogger::Log() << "Missing container name in " << path << std::endl;
                return false;
            }
            if (parameterName == "" and parameterValue == "") {
                TLogger::Log() << "Incorrect container parameter in " << path << ": " << f << std::endl;
                return false;
            } else {
                container[containerName][parameterName] = parameterValue;
            }
        } else {
            if (GetContainerName(f, containerNameTmp)) {
                containerName = containerNameTmp;
            } else {
                TLogger::Log() << "Incorrect container parametr in " << path << ": " << f << std::endl;
                return false;
            }
        }
    }
    for (auto iter: container){
        TLogger::Log() << "Iter: " << iter.first << std::endl;
        containers[iter.first] = container[iter.first];
    }
    return true;
}

// TODO: rename method name
static int StartContainers() {
    TFolder f(CONFIG_DIR);

    std::vector<std::string> files;
    TError error = f.Items(EFileType::Regular, files);
    if (error) {
        L_ERR() << "Can't read config directory: " << error << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<std::string> configs;
    for (auto &f : files) {
        if (extension.length() >= f.length())
            continue;

        if (f.compare(f.length() - extension.length(), extension.length(), extension) != 0)
            continue;

        configs.push_back(f);
    }

    std::sort(configs.begin(), configs.end());

    for (auto &config : configs) {
        std::string filePath = CONFIG_DIR + "/" + config;

        TLogger::Log() << "Loading " << filePath << std::endl;
        init::TConfig cfg;
        if (!LoadConfig(filePath, cfg, containers)) {
            TLogger::Log() << "Failed" << std::endl;
            continue;
        } else {
             TLogger::Log() << "Loaded" << std::endl;
        }

        ExecConfig(cfg);
    }


    TLogger::Log() << "Run!!!!" << std::endl;
    for (auto iter: containers) {
        TLogger::Log() <<  iter.first << std::endl;
        for (auto i: iter.second) {
            TLogger::Log() << i.first << " " << i.second << std::endl;
        }
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

    TLogger::InitLog("", 0);
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
