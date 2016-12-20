#pragma once

#include <map>
#include <vector>
#include <string>
#include <memory>

namespace Porto {

struct Property {
    std::string Name;
    std::string Description;
};

struct Volume {
    std::string Path;
    std::map<std::string, std::string> Properties;
    std::vector<std::string> Containers;
};

struct GetResponse {
    std::string Value;
    int Error;
    std::string ErrorMsg;
};

class Connection {
    class ConnectionImpl;

    std::unique_ptr<ConnectionImpl> Impl;

    Connection(const Connection&) = delete;
    void operator=(const Connection&) = delete;

public:
    Connection();
    ~Connection();

    /* each rpc call does auto-connect/reconnect */
    int Connect();
    void Close();

    /* request timeout in seconds */
    int SetTimeout(int timeout);

    int Create(const std::string &name);
    int CreateWeakContainer(const std::string &name);
    int Destroy(const std::string &name);

    int Start(const std::string &name);
    int Stop(const std::string &name, int timeout = -1);
    int Kill(const std::string &name, int sig);
    int Pause(const std::string &name);
    int Resume(const std::string &name);

    int WaitContainers(const std::vector<std::string> &containers,
                       std::string &name, int timeout);

    int List(std::vector<std::string> &list, const std::string &mask = "");
    int Plist(std::vector<Property> &list);
    int Dlist(std::vector<Property> &list);

    int Get(const std::vector<std::string> &name,
            const std::vector<std::string> &variable,
            std::map<std::string, std::map<std::string, GetResponse>> &result,
            bool nonblock = false);

    int GetProperty(const std::string &name,
            const std::string &property, std::string &value);
    int SetProperty(const std::string &name,
            const std::string &property, std::string value);

    int GetData(const std::string &name,
            const std::string &data, std::string &value);
    int GetVersion(std::string &tag, std::string &revision);

    int Raw(const std::string &message, std::string &response);
    void GetLastError(int &error, std::string &msg) const;

    int ListVolumeProperties(std::vector<Property> &list);
    int CreateVolume(const std::string &path,
                     const std::map<std::string, std::string> &config,
                     Volume &result);
    int CreateVolume(std::string &path,
                     const std::map<std::string, std::string> &config);
    int LinkVolume(const std::string &path,
            const std::string &container = std::string());
    int UnlinkVolume(const std::string &path,
            const std::string &container = std::string());
    int ListVolumes(const std::string &path, const std::string &container,
                    std::vector<Volume> &volumes);
    int ListVolumes(std::vector<Volume> &volumes) {
        return ListVolumes(std::string(), std::string(), volumes);
    }
    int TuneVolume(const std::string &path,
                   const std::map<std::string, std::string> &config);

    int ImportLayer(const std::string &layer, const std::string &tarball,
                    bool merge = false, const std::string &place = "",
                    const std::string &private_value = "");

    int ExportLayer(const std::string &volume, const std::string &tarball);
    int RemoveLayer(const std::string &layer, const std::string &place = "");
    int ListLayers(std::vector<std::string> &layers,
                   const std::string &place = "");

    int GetLayerPrivate(std::string &private_value, const std::string &layer,
                        const std::string &place = "");
    int SetLayerPrivate(const std::string &private_value, const std::string &layer,
                        const std::string &place = "");

    int ConvertPath(const std::string &path, const std::string &src,
                    const std::string &dest, std::string &res);

    int AttachProcess(const std::string &name,
                      int pid, const std::string &comm);
};

} /* namespace Porto */
