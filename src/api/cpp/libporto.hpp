#pragma once

#include <map>
#include <vector>
#include <string>
#include <memory>
#include <functional>

#include "rpc.pb.h"

namespace Porto {

using Porto::rpc::EError;

constexpr int INFINITE_TIMEOUT = 0;
constexpr int DEFAULT_TIMEOUT = 300;        // 5min
constexpr int DEFAULT_DISK_TIMEOUT = 900;   // 15min

constexpr char SOCKET_PATH[] = "/run/portod.socket";

typedef std::function<void(const rpc::TContainerWaitResponse &event)> TWaitCallback;

enum {
    GET_NONBLOCK = 1,
    GET_SYNC = 2,
    GET_REAL = 4,
};

class Connection {
private:
    int Fd = -1;
    int Timeout = DEFAULT_TIMEOUT;
    int DiskTimeout = DEFAULT_DISK_TIMEOUT;

    EError LastError = EError::Success;
    std::string LastErrorMsg;

    rpc::TContainerRequest Req;
    rpc::TContainerResponse Rsp;

    std::vector<std::string> AsyncWaitNames;
    std::vector<std::string> AsyncWaitLabels;
    int AsyncWaitTimeout = -1;
    TWaitCallback AsyncWaitCallback;

    EError SetError(const std::string &prefix, int _errno);

    EError SetSocketTimeout(int direction, int timeout);

    EError Send(const rpc::TContainerRequest &req);

    EError Recv(rpc::TContainerResponse &rsp);

    EError Call(int extra_timeout = -1);

public:
    Connection() { }
    ~Connection();

    int GetFd() const { return Fd; }

    EError Connect(const char *socket_path = SOCKET_PATH);
    void Close();

    /* Request and response timeout in seconds */
    int GetTimeout() const { return Timeout; }
    EError SetTimeout(int timeout);

    /* Extra timeout for disk operations in seconds */
    int GetDiskTimeout() const { return DiskTimeout; }
    EError SetDiskTimeout(int timeout);

    EError Error() const { return LastError; }

    EError GetLastError(std::string &msg) const {
        msg = LastErrorMsg;
        return LastError;
    }

    /* Returns "LastError:(LastErrorMsg)" */
    std::string GetLastError() const;

    /* Returns text protobuf */
    std::string GetLastRequest() const { return Req.DebugString(); }
    std::string GetLastResponse() const { return Rsp.DebugString(); }

    EError Call(const rpc::TContainerRequest &req,
                rpc::TContainerResponse &rsp,
                int extra_timeout = -1);

    EError Call(const std::string &req,
                std::string &rsp,
                int extra_timeout = -1);

    /* System */

    EError GetVersion(std::string &tag, std::string &revision);

    const rpc::TGetSystemResponse *GetSystem();

    EError SetSystem(const std::string &key, const std::string &val);

    /* Container */

    const rpc::TContainerPropertyListResponse *ListProperties();

    EError ListProperties(std::vector<std::string> &properties);

    const rpc::TContainerListResponse *List(const std::string &mask = "");

    EError List(std::vector<std::string> &names, const std::string &mask = "");

    EError Create(const std::string &name);

    EError CreateWeakContainer(const std::string &name);

    EError Destroy(const std::string &name);

    EError Start(const std::string &name);

    EError Stop(const std::string &name, int stop_timeout = -1);

    EError Kill(const std::string &name, int sig = 9);

    EError Pause(const std::string &name);

    EError Resume(const std::string &name);

    EError Respawn(const std::string &name);

    EError WaitContainer(const std::string &name,
                         std::string &result_state,
                         int wait_timeout = -1);

    EError WaitContainers(const std::vector<std::string> &names,
                          std::string &result_name,
                          std::string &result_state,
                          int wait_timeout = -1);

    const rpc::TContainerWaitResponse *Wait(const std::vector<std::string> &names,
                                            const std::vector<std::string> &labels,
                                            int wait_timeout = -1);

    EError AsyncWait(const std::vector<std::string> &names,
                     const std::vector<std::string> &labels,
                     TWaitCallback callbacks,
                     int wait_timeout = -1);

    void RecvAsyncWait() {
        Recv(Rsp);
    }

    const rpc::TContainerGetResponse *Get(const std::vector<std::string> &names,
                                          const std::vector<std::string> &properties,
                                          int flags = 0);

    /* Porto v5 api */
    const rpc::TContainerSpec *GetContainerSpec(const std::string &name);

    EError GetProperty(const std::string &name,
                       const std::string &property,
                       std::string &value,
                       int flags = 0);

    EError GetProperty(const std::string &name,
                       const std::string &property,
                       uint64_t &value,
                       int flags = 0);

    EError SetProperty(const std::string &name,
                       const std::string &property,
                       const std::string &value);

    EError IncLabel(const std::string &name,
                    const std::string &label,
                    int64_t add,
                    int64_t &result);

    EError IncLabel(const std::string &name,
                    const std::string &label,
                    int64_t add = 1) {
        int64_t result;
        return IncLabel(name, label, add, result);
    }

    EError ConvertPath(const std::string &path,
                       const std::string &src_name,
                       const std::string &dst_name,
                       std::string &result_path);

    EError AttachProcess(const std::string &name, int pid,
                         const std::string &comm = "");

    EError AttachThread(const std::string &name, int pid,
                        const std::string &comm = "");

    EError LocateProcess(int pid, const std::string &comm /* = "" */,
                         std::string &name);

    /* Volume */

    const rpc::TVolumePropertyListResponse *ListVolumeProperties();

    EError ListVolumeProperties(std::vector<std::string> &properties);

    const rpc::TVolumeListResponse *ListVolumes(const std::string &path = "",
                                                const std::string &container = "");

    EError ListVolumes(std::vector<std::string> &paths);

    const rpc::TVolumeDescription *GetVolume(const std::string &path);

    /* Porto v5 api */
    const rpc::TVolumeSpec *GetVolumeSpec(const std::string &path);

    EError CreateVolume(std::string &path,
                        const std::map<std::string, std::string> &config);

    EError LinkVolume(const std::string &path,
                      const std::string &container = "",
                      const std::string &target = "",
                      bool read_only = false,
                      bool required = false);

    EError UnlinkVolume(const std::string &path,
                        const std::string &container = "",
                        const std::string &target = "***",
                        bool strict = false);

    EError TuneVolume(const std::string &path,
                      const std::map<std::string, std::string> &config);

    /* Layer */

    const rpc::TLayerListResponse *ListLayers(const std::string &place = "",
                                              const std::string &mask = "");

    EError ListLayers(std::vector<std::string> layers,
                      const std::string &place = "",
                      const std::string &mask = "");

    EError ImportLayer(const std::string &layer,
                       const std::string &tarball,
                       bool merge = false,
                       const std::string &place = "",
                       const std::string &private_value = "");

    EError ExportLayer(const std::string &volume,
                       const std::string &tarball,
                       const std::string &compress = "");

    EError ReExportLayer(const std::string &layer,
                         const std::string &tarball,
                         const std::string &compress = "");

    EError RemoveLayer(const std::string &layer,
                       const std::string &place = "");

    EError GetLayerPrivate(std::string &private_value,
                           const std::string &layer,
                           const std::string &place = "");

    EError SetLayerPrivate(const std::string &private_value,
                           const std::string &layer,
                           const std::string &place = "");

    /* Storage */

    const rpc::TStorageListResponse *ListStorages(const std::string &place = "",
                                                  const std::string &mask = "");

    EError ListStorages(std::vector<std::string> &storages,
                        const std::string &place = "",
                        const std::string &mask = "");

    EError RemoveStorage(const std::string &storage,
                         const std::string &place = "");

    EError ImportStorage(const std::string &storage,
                         const std::string &archive,
                         const std::string &place = "",
                         const std::string &compression = "",
                         const std::string &private_value = "");

    EError ExportStorage(const std::string &storage,
                         const std::string &archive,
                         const std::string &place = "",
                         const std::string &compression = "");
};

} /* namespace Porto */
