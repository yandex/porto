#include <vector>
#include <string>
#include <map>
#include <algorithm>

#include "init.pb.h"

//#include "util/protobuf.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/folder.hpp"
#include "util/file.hpp"
#include "api/cpp/libporto.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

static volatile sig_atomic_t reloadConfigs = true;
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

//static void CreateHook() {
//
//}

//static bool RunHook(std::string &containerName, std::string &hookName, std::string &hookValue, TPortoAPI &api) {
//    CreateHook();
//    StartHook();
//    WaitHook();
//    DestroyHook();
//}

static void StartContainers (std::map<std::string, std::map<std::string, std::string>> &containers, TPortoAPI &api) {
    int ret, error;
    std::string msg;
    TLogger::Log() << "Run!!!!" << std::endl;
    for (auto iter: containers) {
        std::string s, containerName = "portoinit@" + iter.first;

        TLogger::Log() << containerName << std::endl;
// create container
        ret = api.GetData(containerName, "state", s);
        if (ret) {
            api.GetLastError(error, msg);
            L_ERR() << "Can't get container state" << std::endl;
            L_ERR() << msg << std::endl;
        }

        if (s == "dead") {
            TLogger::Log() << "Destroy and reload: " << containerName << std::endl;
            ret = api.Destroy(containerName);
            if (ret) {
                api.GetLastError(error, msg);
                L_ERR() << msg << std::endl;
            }
        }
        if (s != "running" and s != "stopped") {
            ret = api.Create(containerName);
            if (ret) {
                api.GetLastError(error, msg);
                L_ERR() << "Can't create container: " << containerName << std::endl;
                L_ERR() << msg << std::endl;
                continue;
            }
        }
// set container property
        for (auto i: iter.second) {
            std::string p;

            ret = api.GetProperty(containerName, i.first, p);
            if (ret) {
                api.GetLastError(error, msg);
                L_ERR() << "Can't get container property" << std::endl;
                L_ERR() << msg << std::endl;
            }
            if (p != i.second) {
                TLogger::Log() << i.first << " " << i.second << std::endl;
                ret = api.SetProperty(containerName, i.first, i.second);
                if (ret) {
                    api.GetLastError(error, msg);
                    if (error == 8) {
                        ret = api.Stop(containerName);
                        if (ret) {
                            L_ERR() << msg << " " << error << std::endl;
                        } else {
                            ret = api.SetProperty(containerName, i.first, i.second);
                            if (ret)
                                L_ERR() << msg << " " << error << std::endl;
                        }
                    } else {
                        L_ERR() << "Can't set property to " << containerName << ": " <<  i.first << "=" << i.second << std::endl;
                        L_ERR() << msg << " " << error << std::endl;
                    }
                }
            }
        }
// start container
        if (s != "running") {
            ret = api.Start(containerName);
            if (ret) {
                api.GetLastError(error, msg);
                L_ERR() << "Can't start container: " << containerName << std::endl;
                L_ERR() << msg << std::endl;
            }
        }
    }
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

static void ReloadConfigs(int signum)
{
    reloadConfigs = true;
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

    (void)RegisterSignal(SIGHUP, ReloadConfigs);

    config.Load(true);
    TPortoAPI api(config().rpc_sock().file().path());

    for (;;) {
        if (reloadConfigs) {
            GetConfigs(configs);
            LoadConfigs(configs, containers);
            reloadConfigs = false;
        }
        StartContainers(containers, api);
        usleep(60000000);
    }

    EventLoop();

    StopPorto();
    TLogger::CloseLog();

    CleanupSystem();

    if (NeedRestart)
        Restart();
    Poweroff();

    return EXIT_SUCCESS;
}
