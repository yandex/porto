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
    TString LastErrorMsg;

    rpc::TPortoRequest Req;
    rpc::TPortoResponse Rsp;

    std::vector<TString> AsyncWaitNames;
    std::vector<TString> AsyncWaitLabels;
    int AsyncWaitTimeout = -1;
    TWaitCallback AsyncWaitCallback;

    EError SetError(const TString &prefix, int _errno);

    EError SetSocketTimeout(int direction, int timeout);

    EError Send(const rpc::TPortoRequest &req);

    EError Recv(rpc::TPortoResponse &rsp);

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

    EError GetLastError(TString &msg) const {
        msg = LastErrorMsg;
        return LastError;
    }

    /* Returns "LastError:(LastErrorMsg)" */
    TString GetLastError() const;

    /* Returns text protobuf */
    TString GetLastRequest() const { return Req.DebugString(); }
    TString GetLastResponse() const { return Rsp.DebugString(); }

    EError Call(const rpc::TPortoRequest &req,
                rpc::TPortoResponse &rsp,
                int extra_timeout = -1);

    EError Call(const TString &req,
                TString &rsp,
                int extra_timeout = -1);

    /* System */

    EError GetVersion(TString &tag, TString &revision);

    const rpc::TGetSystemResponse *GetSystem();

    EError SetSystem(const TString &key, const TString &val);

    /* Container */

    const rpc::TContainerPropertyListResponse *ListProperties();

    EError ListProperties(std::vector<TString> &properties);

    const rpc::TContainerListResponse *List(const TString &mask = "");

    EError List(std::vector<TString> &names, const TString &mask = "");

    EError Create(const TString &name);

    EError CreateWeakContainer(const TString &name);

    EError Destroy(const TString &name);

    EError Start(const TString &name);

    EError Stop(const TString &name, int stop_timeout = -1);

    EError Kill(const TString &name, int sig = 9);

    EError Pause(const TString &name);

    EError Resume(const TString &name);

    EError Respawn(const TString &name);

    EError WaitContainer(const TString &name,
                         TString &result_state,
                         int wait_timeout = -1);

    EError WaitContainers(const std::vector<TString> &names,
                          TString &result_name,
                          TString &result_state,
                          int wait_timeout = -1);

    const rpc::TContainerWaitResponse *Wait(const std::vector<TString> &names,
                                            const std::vector<TString> &labels,
                                            int wait_timeout = -1);

    EError AsyncWait(const std::vector<TString> &names,
                     const std::vector<TString> &labels,
                     TWaitCallback callbacks,
                     int wait_timeout = -1);

    void RecvAsyncWait() {
        Recv(Rsp);
    }

    const rpc::TContainerGetResponse *Get(const std::vector<TString> &names,
                                          const std::vector<TString> &properties,
                                          int flags = 0);

    /* Porto v5 api */
    const rpc::TContainerSpec *GetContainerSpec(const TString &name);

    EError GetProperty(const TString &name,
                       const TString &property,
                       TString &value,
                       int flags = 0);

    EError GetProperty(const TString &name,
                       const TString &property,
                       uint64_t &value,
                       int flags = 0);

    EError SetProperty(const TString &name,
                       const TString &property,
                       const TString &value);

    EError IncLabel(const TString &name,
                    const TString &label,
                    int64_t add,
                    int64_t &result);

    EError IncLabel(const TString &name,
                    const TString &label,
                    int64_t add = 1) {
        int64_t result;
        return IncLabel(name, label, add, result);
    }

    EError ConvertPath(const TString &path,
                       const TString &src_name,
                       const TString &dst_name,
                       TString &result_path);

    EError AttachProcess(const TString &name, int pid,
                         const TString &comm = "");

    EError AttachThread(const TString &name, int pid,
                        const TString &comm = "");

    EError LocateProcess(int pid, const TString &comm /* = "" */,
                         TString &name);

    /* Volume */

    const rpc::TVolumePropertyListResponse *ListVolumeProperties();

    EError ListVolumeProperties(std::vector<TString> &properties);

    const rpc::TVolumeListResponse *ListVolumes(const TString &path = "",
                                                const TString &container = "");

    EError ListVolumes(std::vector<TString> &paths);

    const rpc::TVolumeDescription *GetVolume(const TString &path);

    /* Porto v5 api */
    const rpc::TVolumeSpec *GetVolumeSpec(const TString &path);

    EError CreateVolume(TString &path,
                        const std::map<TString, TString> &config);

    EError LinkVolume(const TString &path,
                      const TString &container = "",
                      const TString &target = "",
                      bool read_only = false,
                      bool required = false);

    EError UnlinkVolume(const TString &path,
                        const TString &container = "",
                        const TString &target = "***",
                        bool strict = false);

    EError TuneVolume(const TString &path,
                      const std::map<TString, TString> &config);

    /* Layer */

    const rpc::TLayerListResponse *ListLayers(const TString &place = "",
                                              const TString &mask = "");

    EError ListLayers(std::vector<TString> layers,
                      const TString &place = "",
                      const TString &mask = "");

    EError ImportLayer(const TString &layer,
                       const TString &tarball,
                       bool merge = false,
                       const TString &place = "",
                       const TString &private_value = "");

    EError ExportLayer(const TString &volume,
                       const TString &tarball,
                       const TString &compress = "");

    EError ReExportLayer(const TString &layer,
                         const TString &tarball,
                         const TString &compress = "");

    EError RemoveLayer(const TString &layer,
                       const TString &place = "");

    EError GetLayerPrivate(TString &private_value,
                           const TString &layer,
                           const TString &place = "");

    EError SetLayerPrivate(const TString &private_value,
                           const TString &layer,
                           const TString &place = "");

    /* Storage */

    const rpc::TStorageListResponse *ListStorages(const TString &place = "",
                                                  const TString &mask = "");

    EError ListStorages(std::vector<TString> &storages,
                        const TString &place = "",
                        const TString &mask = "");

    EError RemoveStorage(const TString &storage,
                         const TString &place = "");

    EError ImportStorage(const TString &storage,
                         const TString &archive,
                         const TString &place = "",
                         const TString &compression = "",
                         const TString &private_value = "");

    EError ExportStorage(const TString &storage,
                         const TString &archive,
                         const TString &place = "",
                         const TString &compression = "");
};

} /* namespace Porto */
