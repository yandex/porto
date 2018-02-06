#pragma once

#include <map>
#include <vector>
#include <string>
#include <memory>

namespace rpc {
    class TContainerRequest;
    class TContainerResponse;
}

namespace Porto {

struct Property {
    std::string Name;
    std::string Description;
};

struct Volume {
    std::string Path;
    std::map<std::string, std::string> Properties;
    std::map<std::string, std::string> Links;
};

struct Layer {
    std::string Name;
    std::string OwnerUser;
    std::string OwnerGroup;
    std::string PrivateValue;
    uint64_t LastUsage;
};

struct Storage {
    std::string Name;
    std::string OwnerUser;
    std::string OwnerGroup;
    std::string PrivateValue;
    uint64_t LastUsage;
};

struct GetResponse {
    std::string Value;
    int Error;
    std::string ErrorMsg;
};

enum GetFlags {
    NonBlock = 1,
    Sync = 2,
    Real = 4,
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

    int List(std::vector<std::string> &list,
             const std::string &mask = "");
    int Plist(std::vector<Property> &list);
    int Dlist(std::vector<Property> &list);

    int Get(const std::vector<std::string> &name,
            const std::vector<std::string> &variable,
            std::map<std::string, std::map<std::string, GetResponse>> &result,
            int flags = 0);

    int GetProperty(const std::string &name,
            const std::string &property, std::string &value, int flags = 0);
    int SetProperty(const std::string &name,
            const std::string &property, std::string value);

    int GetData(const std::string &name, const std::string &property,
                std::string &value) {
        return GetProperty(name, property, value);
    }

    int GetVersion(std::string &tag, std::string &revision);

    int Rpc(const rpc::TContainerRequest &req, rpc::TContainerResponse &rsp);
    int Raw(const std::string &message, std::string &response);
    void GetLastError(int &error, std::string &msg) const;
    std::string TextError() const;

    int ListVolumeProperties(std::vector<Property> &list);
    int CreateVolume(const std::string &path,
                     const std::map<std::string, std::string> &config,
                     Volume &result);
    int CreateVolume(std::string &path,
                     const std::map<std::string, std::string> &config);
    int LinkVolume(const std::string &path,
                   const std::string &container = "",
                   const std::string &target = "",
                   bool required = false);
    int UnlinkVolume(const std::string &path,
                     const std::string &container = "", bool strict = false);
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

    int ExportLayer(const std::string &volume, const std::string &tarball,
                    const std::string &compress = "");
    int RemoveLayer(const std::string &layer, const std::string &place = "");
    int ListLayers(std::vector<Layer> &layers,
                   const std::string &place = "",
                   const std::string &mask = "");

    int GetLayerPrivate(std::string &private_value, const std::string &layer,
                        const std::string &place = "");
    int SetLayerPrivate(const std::string &private_value, const std::string &layer,
                        const std::string &place = "");

    int ListStorage(std::vector<Storage> &storages,
                    const std::string &place = "",
                    const std::string &mask = "");
    int RemoveStorage(const std::string &name, const std::string &place = "");
    int ImportStorage(const std::string &name,
                      const std::string &archive,
                      const std::string &place = "",
                      const std::string &compression = "",
                      const std::string &private_value = "");
    int ExportStorage(const std::string &name,
                      const std::string &archive,
                      const std::string &place = "",
                      const std::string &compression = "");

    int ConvertPath(const std::string &path, const std::string &src,
                    const std::string &dest, std::string &res);

    int AttachProcess(const std::string &name,
                      int pid, const std::string &comm);
    int LocateProcess(int pid, const std::string &comm, std::string &name);
};

} /* namespace Porto */
