#pragma once

#include <map>
#include <vector>
#include <string>
#include <memory>

struct TProperty {
    std::string Name;
    std::string Description;

    TProperty(std::string name, std::string description) :
        Name(name), Description(description) {}
};

struct TData {
    std::string Name;
    std::string Description;

    TData(std::string name, std::string description) :
        Name(name), Description(description) {}
};

struct TVolumeDescription {
    std::string Path;
    std::map<std::string, std::string> Properties;
    std::vector<std::string> Containers;

    TVolumeDescription() {}
    TVolumeDescription(const std::string &path,
                       const std::map<std::string, std::string> &properties,
                       const std::vector<std::string> &containers) :
        Path(path), Properties(properties), Containers(containers) {}
};

struct TPortoGetResponse {
    std::string Value;
    int Error;
    std::string ErrorMsg;
};

class TPortoAPIImpl;

class TPortoAPI {
    std::unique_ptr<TPortoAPIImpl> Impl;

public:
    TPortoAPI(const std::string &path, int retries = 5);
    ~TPortoAPI();

    void Cleanup(); // TODO: remove

    int Create(const std::string &name);
    int Destroy(const std::string &name);

    int Start(const std::string &name);
    int Stop(const std::string &name);
    int Kill(const std::string &name, int sig);
    int Pause(const std::string &name);
    int Resume(const std::string &name);

    int Wait(const std::vector<std::string> &containers, std::string &name, int timeout = -1);

    int List(std::vector<std::string> &clist);
    int Plist(std::vector<TProperty> &plist);
    int Dlist(std::vector<TData> &dlist);

    int Get(const std::vector<std::string> &name,
            const std::vector<std::string> &variable,
            std::map<std::string, std::map<std::string, TPortoGetResponse>> &result);

    int GetProperty(const std::string &name, const std::string &property, std::string &value);
    int SetProperty(const std::string &name, const std::string &property, std::string value);

    int GetData(const std::string &name, const std::string &data, std::string &value);
    int GetVersion(std::string &tag, std::string &revision);

    int Raw(const std::string &message, std::string &response);
    void GetLastError(int &error, std::string &msg) const;

    // VolumeAPI
    int ListVolumeProperties(std::vector<TProperty> &properties);
    int CreateVolume(const std::string &path,
                     const std::map<std::string, std::string> &config,
                     TVolumeDescription &result);
    int CreateVolume(std::string &path,
                     const std::map<std::string, std::string> &config);
    int LinkVolume(const std::string &path, const std::string &container = "");
    int UnlinkVolume(const std::string &path, const std::string &container = "");
    int ListVolumes(const std::string &path, const std::string &container,
                    std::vector<TVolumeDescription> &volumes);
    int ListVolumes(std::vector<TVolumeDescription> &volumes) {
        return ListVolumes("", "", volumes);
    }

    int ImportLayer(const std::string &layer, const std::string &tarball, bool merge = false);
    int ExportLayer(const std::string &volume, const std::string &tarball);
    int RemoveLayer(const std::string &layer);
    int ListLayers(std::vector<std::string> &layers);
};
