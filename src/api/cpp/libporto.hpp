#pragma once

#include <map>
#include <vector>
#include <string>
#include <memory>

struct TPortoProperty {
    std::string Name;
    std::string Description;
};

struct TPortoVolume {
    std::string Path;
    std::map<std::string, std::string> Properties;
    std::vector<std::string> Containers;
};

struct TPortoGetResponse {
    std::string Value;
    int Error;
    std::string ErrorMsg;
};

class TPortoAPI {
    class TPortoAPIImpl;

    std::unique_ptr<TPortoAPIImpl> Impl;

    TPortoAPI(const TPortoAPI&) = delete;
    void operator=(const TPortoAPI&) = delete;

public:
    TPortoAPI();
    ~TPortoAPI();

    /* each rpc call does auto-connect/reconnect */
    int Connect();
    void Close();

    int Create(const std::string &name);
    int CreateWeakContainer(const std::string &name);
    int Destroy(const std::string &name);

    int Start(const std::string &name);
    int Stop(const std::string &name);
    int Kill(const std::string &name, int sig);
    int Pause(const std::string &name);
    int Resume(const std::string &name);

    int Wait(const std::vector<std::string> &containers,
             std::string &name, int timeout = -1);

    int List(std::vector<std::string> &clist);
    int Plist(std::vector<TPortoProperty> &list);
    int Dlist(std::vector<TPortoProperty> &list);

    int Get(const std::vector<std::string> &name,
            const std::vector<std::string> &variable,
            std::map<std::string, std::map<std::string,
                            TPortoGetResponse>> &result);

    int GetProperty(const std::string &name,
            const std::string &property, std::string &value);
    int SetProperty(const std::string &name,
            const std::string &property, std::string value);

    int GetData(const std::string &name,
            const std::string &data, std::string &value);
    int GetVersion(std::string &tag, std::string &revision);

    int Raw(const std::string &message, std::string &response);
    void GetLastError(int &error, std::string &msg) const;

    int ListVolumeProperties(std::vector<TPortoProperty> &list);
    int CreateVolume(const std::string &path,
                     const std::map<std::string, std::string> &config,
                     TPortoVolume &result);
    int CreateVolume(std::string &path,
                     const std::map<std::string, std::string> &config);
    int LinkVolume(const std::string &path,
            const std::string &container = std::string());
    int UnlinkVolume(const std::string &path,
            const std::string &container = std::string());
    int ListVolumes(const std::string &path, const std::string &container,
                    std::vector<TPortoVolume> &volumes);
    int ListVolumes(std::vector<TPortoVolume> &volumes) {
        return ListVolumes(std::string(), std::string(), volumes);
    }
    int TuneVolume(const std::string &path,
                   const std::map<std::string, std::string> &config);

    int ImportLayer(const std::string &layer, const std::string &tarball,
                    bool merge = false);
    int ExportLayer(const std::string &volume, const std::string &tarball);
    int RemoveLayer(const std::string &layer);
    int ListLayers(std::vector<std::string> &layers);

    int ConvertPath(const std::string &path, const std::string &src,
                    const std::string &dest, std::string &res);
};
