#pragma once

#include <map>
#include <vector>
#include <string>
#include <memory>
#include <functional>

namespace rpc {
    class TContainerRequest;
    class TContainerResponse;
    class TStorageListResponse;
}

namespace Porto {

constexpr int INFINITE_TIMEOUT = -1;
constexpr int DEFAULT_TIMEOUT = 300;    // 5min
constexpr int DISK_TIMEOUT = 900;       // 15min

struct Property {
    std::string Name;
    std::string Description;
    bool ReadOnly = false;
    bool Dynamic = false;
};

struct VolumeLink {
    std::string Container;
    std::string Target;
    bool ReadOnly = false;
    bool Required = false;
};

struct Volume {
    std::string Path;
    std::map<std::string, std::string> Properties;
    std::vector<VolumeLink> Links;
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

struct AsyncWaitEvent {
    time_t When;
    std::string Name;
    std::string State;
    std::string Label;
    std::string Value;
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

    /* request and response timeout in seconds */
    int GetTimeout() const;
    int SetTimeout(int timeout);

    /* extra timeout for disk operations in seconds */
    int GetDiskTimeout() const;
    int SetDiskTimeout(int timeout);

    std::string GetLastError() const;
    void GetLastError(int &error, std::string &msg) const;

    int Call(const rpc::TContainerRequest &req, rpc::TContainerResponse &rsp,
             int extra_timeout = 0);
    int Call(const std::string &req, std::string &rsp, int extra_timeout = 0);

    int Create(const std::string &name);
    int CreateWeakContainer(const std::string &name);
    int Destroy(const std::string &name);

    int Start(const std::string &name);
    int Stop(const std::string &name, int timeout = -1);
    int Kill(const std::string &name, int sig);
    int Pause(const std::string &name);
    int Resume(const std::string &name);
    int Respawn(const std::string &name);

    int WaitContainers(const std::vector<std::string> &containers,
                       const std::vector<std::string> &labels,
                       std::string &name, int timeout = INFINITE_TIMEOUT);

    int AsyncWait(const std::vector<std::string> &containers,
                  const std::vector<std::string> &labels,
                  std::function<void(AsyncWaitEvent &event)> callbacks,
                  int timeout = INFINITE_TIMEOUT);
    int Recv();

    int List(std::vector<std::string> &list,
             const std::string &mask = "");
    int ListProperties(std::vector<Property> &list);

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

    int IncLabel(const std::string &name, const std::string &label,
                 int64_t add, int64_t &result);

    int IncLabel(const std::string &name, const std::string &label,
                 int64_t add = 1) {
        int64_t result;
        return IncLabel(name, label, add, result);
    }

    int GetVersion(std::string &tag, std::string &revision);

    int ListVolumeProperties(std::vector<Property> &list);
    int CreateVolume(const std::string &path,
                     const std::map<std::string, std::string> &config,
                     Volume &result);
    int CreateVolume(std::string &path,
                     const std::map<std::string, std::string> &config);
    int LinkVolume(const std::string &path,
                   const std::string &container = "",
                   const std::string &target = "",
                   bool read_only = false,
                   bool required = false);
    int UnlinkVolume(const std::string &path,
                     const std::string &container = "",
                     const std::string &target = "***",
                     bool strict = false);
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

    const rpc::TStorageListResponse *ListStorage(const std::string &place = "", const std::string &mask = "");

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
    int AttachThread(const std::string &name,
                     int pid, const std::string &comm);
    int LocateProcess(int pid, const std::string &comm, std::string &name);
};

} /* namespace Porto */
