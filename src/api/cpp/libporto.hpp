#pragma once

#include <map>
#include <vector>
#include <string>
#include <functional>

#include "rpc.pb.h"

namespace Porto {

constexpr int INFINITE_TIMEOUT = -1;
constexpr int DEFAULT_TIMEOUT = 300;        // 5min
constexpr int DEFAULT_DISK_TIMEOUT = 900;   // 15min

constexpr char SOCKET_PATH[] = "/run/portod.socket";

typedef std::string TString;

typedef std::function<void(const TWaitResponse &event)> TWaitCallback;

enum {
    GET_NONBLOCK = 1,
    GET_SYNC = 2,
    GET_REAL = 4,
};

class TPortoApi {
private:
    int Fd = -1;
    int Timeout = DEFAULT_TIMEOUT;
    int DiskTimeout = DEFAULT_DISK_TIMEOUT;
    bool AutoReconnect = true;

    EError LastError = EError::Success;
    TString LastErrorMsg;

    /*
     * These keep last request and response. Method might return
     * pointers to Rsp innards -> pointers valid until next call.
     */
    TPortoRequest Req;
    TPortoResponse Rsp;

    std::vector<TString> AsyncWaitNames;
    std::vector<TString> AsyncWaitLabels;
    int AsyncWaitTimeout = INFINITE_TIMEOUT;
    TWaitCallback AsyncWaitCallback;

    EError SetError(const TString &prefix, int _errno);

    EError SetSocketTimeout(int direction, int timeout);

    EError Send(const TPortoRequest &req);

    EError Recv(TPortoResponse &rsp);

    EError Call(int extra_timeout = 0);

    EError CallWait(TString &result_state, int wait_timeout);

public:
    TPortoApi() { }
    ~TPortoApi();

    int GetFd() const { return Fd; }
    bool Connected() const { return Fd >= 0; }

    EError Connect(const char *socket_path = SOCKET_PATH);
    void Disconnect();

    /* Requires signal(SIGPIPE, SIG_IGN) */
    void SetAutoReconnect(bool auto_reconnect) {
        AutoReconnect = auto_reconnect;
    }

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

    /* To be used for next changed_since */
    uint64_t ResponseTimestamp() const { return Rsp.timestamp(); }

    // extra_timeout: 0 - none, -1 - infinite
    EError Call(const TPortoRequest &req,
                TPortoResponse &rsp,
                int extra_timeout = 0);

    EError Call(const TString &req,
                TString &rsp,
                int extra_timeout = 0);

    /* System */

    EError GetVersion(TString &tag, TString &revision);

    const TGetSystemResponse *GetSystem();

    EError SetSystem(const TString &key, const TString &val);

    const TGetSystemConfigResponse *GetSystemConfig();

    /* Container */

    const TListPropertiesResponse *ListProperties();

    EError ListProperties(std::vector<TString> &properties);

    const TListResponse *List(const TString &mask = "");

    EError List(std::vector<TString> &names, const TString &mask = "");

    EError Create(const TString &name);

    EError CreateWeakContainer(const TString &name);

    EError Destroy(const TString &name);

    EError Start(const TString &name);

    // stop_timeout: time between SIGTERM and SIGKILL, -1 - default
    EError Stop(const TString &name, int stop_timeout = -1);

    EError Kill(const TString &name, int sig = 9);

    EError Pause(const TString &name);

    EError Resume(const TString &name);

    EError Respawn(const TString &name);

    // wait_timeout: 0 - nonblock, -1 - infinite
    EError WaitContainer(const TString &name,
                         TString &result_state,
                         int wait_timeout = INFINITE_TIMEOUT);

    EError WaitContainers(const std::vector<TString> &names,
                          TString &result_name,
                          TString &result_state,
                          int wait_timeout = INFINITE_TIMEOUT);

    const TWaitResponse *Wait(const std::vector<TString> &names,
                              const std::vector<TString> &labels,
                              int wait_timeout = INFINITE_TIMEOUT);

    EError AsyncWait(const std::vector<TString> &names,
                     const std::vector<TString> &labels,
                     TWaitCallback callbacks,
                     int wait_timeout = INFINITE_TIMEOUT);

    void RecvAsyncWait() {
        Recv(Rsp);
    }

    const TGetResponse *Get(const std::vector<TString> &names,
                            const std::vector<TString> &properties,
                            int flags = 0);

    /* Porto v5 api */
    const TContainer *GetContainer(const TString &name);

    const TGetContainerResponse *GetContainers(uint64_t changed_since = 0);

    EError GetProperty(const TString &name,
                       const TString &property,
                       TString &value,
                       int flags = 0);

    EError GetProperty(const TString &name,
                       const TString &property,
                       const TString &index,
                       TString &value,
                       int flags = 0) {
        return GetProperty(name, property + "[" + index + "]", value, flags);
    }

    EError SetProperty(const TString &name,
                       const TString &property,
                       const TString &value);

    EError SetProperty(const TString &name,
                       const TString &property,
                       const TString &index,
                       const TString &value) {
        return SetProperty(name, property + "[" + index + "]", value);
    }

    EError GetInt(const TString &name,
                  const TString &property,
                  const TString &index,
                  uint64_t &value);

    EError GetInt(const TString &name,
                       const TString &property,
                       uint64_t &value) {
        return GetInt(name, property, "", value);
    }

    EError SetInt(const TString &name,
                  const TString &property,
                  const TString &index,
                  uint64_t value);

    EError SetInt(const TString &name,
                  const TString &property,
                  uint64_t value) {
        return SetInt(name, property, "", value);
    }

    EError GetLabel(const TString &name,
                    const TString &label,
                    TString &value) {
        return GetProperty(name, "labels", label, value);
    }

    EError SetLabel(const TString &name,
                    const TString &label,
                    const TString &value,
                    const TString &prev_value = " ");

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

    EError LocateProcess(int pid,
                         const TString &comm /* = "" */,
                         TString &name);

    /* Volume */

    const TListVolumePropertiesResponse *ListVolumeProperties();

    EError ListVolumeProperties(std::vector<TString> &properties);

    const TListVolumesResponse *ListVolumes(const TString &path = "",
                                            const TString &container = "");

    EError ListVolumes(std::vector<TString> &paths);

    const TVolumeDescription *GetVolumeDesc(const TString &path);

    /* Porto v5 api */
    const TVolume *GetVolume(const TString &path);

    const TGetVolumeResponse *GetVolumes(uint64_t changed_since = 0);

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

    EError SetVolumeLabel(const TString &path,
                          const TString &label,
                          const TString &value,
                          const TString &prev_value = " ");

    /* Layer */

    const TListLayersResponse *ListLayers(const TString &place = "",
                                          const TString &mask = "");

    EError ListLayers(std::vector<TString> &layers,
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

    const TListStoragesResponse *ListStorages(const TString &place = "",
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
