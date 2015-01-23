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
static std::vector<std::string> configs;
static std::map<std::string, std::map<std::string, std::string>> containers;

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

static int StartContainers (std::map<std::string, std::map<std::string, std::string>> &containers) {
    TLogger::Log() << "Run!!!!" << std::endl;
    for (auto iter: containers) {
        TLogger::Log() <<  iter.first << std::endl;
        for (auto i: iter.second) {
            TLogger::Log() << i.first << " " << i.second << std::endl;
        }
    }

    return 0;
}

static bool LoadConfig(const std::string path, std::map<std::string, std::map<std::string, std::string>> &containers) {
    std::vector<std::string> config;
    std::string containerNameTmp, containerName, parameterName, parameterValue;
    std::map<std::string, std::map<std::string, std::string>> container;

    TFile f(path);
    TError error = f.AsLines(config);
    if (error) {
        L_ERR() << "Can't read config directory: " << error << std::endl;
        return false;
    }
    for(auto &f: config){
        if (f == "") {
            continue;
        }
        if (GetContainerProperty(f, parameterName, parameterValue)) {
            if(containerName == "") {
                L_ERR() << "Missing container name in " << path << std::endl;
                return false;
            }
            if (parameterName == "" and parameterValue == "") {
                L_ERR() << "Incorrect container parameter in " << path << ": " << f << std::endl;
                return false;
            } else {
                container[containerName][parameterName] = parameterValue;
            }
        } else {
            if (GetContainerName(f, containerNameTmp)) {
                containerName = containerNameTmp;
            } else {
                L_ERR() << "Incorrect container parametr in " << path << ": " << f << std::endl;
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

static void LoadConfigs (std::vector<std::string> &configs, std::map<std::string, std::map<std::string, std::string>> &containers)
{
    for (auto &config : configs) {
        std::string filePath = CONFIG_DIR + "/" + config;

        TLogger::Log() << "Loading: " << filePath << std::endl;

        if (!LoadConfig(filePath, containers)) {
            L_ERR() << "Failed: " << filePath << std::endl;
            continue;
        } else {
             TLogger::Log() << "Loaded: " << filePath << std::endl;
        }
    }
}

static void GetConfigs (std::vector<std::string> &configs)
{
    TFolder f(CONFIG_DIR);

    std::vector<std::string> files;
    TError error = f.Items(EFileType::Regular, files);
    if (error) {
        L_ERR() << "Can't read config directory: " << error << std::endl;
    }

    for (auto &f : files) {
        if (extension.length() >= f.length())
            continue;

        if (f.compare(f.length() - extension.length(), extension.length(), extension) != 0)
            continue;

        configs.push_back(f);
    }

    std::sort(configs.begin(), configs.end());
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

    GetConfigs(configs);
    LoadConfigs(configs, containers);
    StartContainers(containers);

    EventLoop();

    StopPorto();
    TLogger::CloseLog();

    CleanupSystem();

    if (NeedRestart)
        Restart();
    Poweroff();

    return EXIT_SUCCESS;
}
