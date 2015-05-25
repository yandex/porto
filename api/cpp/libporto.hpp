#pragma once

#include "rpc.hpp"
#include "util/protobuf.hpp"

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
    std::string Source;
    std::string Quota;
    std::string Flags;
    uint64_t Used;
    uint64_t Avail;

    TVolumeDescription(const std::string &path, const std::string &source,
                       const std::string &quota, const std::string &flags,
                       uint64_t used, uint64_t avail) :
        Path(path), Source(source), Quota(quota), Flags(flags), Used(used), Avail(avail) {}
};

struct TPortoGetResponse {
    std::string Value;
    int Error;
    std::string ErrorMsg;
};

class TPortoAPI {
    int Fd;
    const int Retries;
    const int RetryDelayUs = 1000000;
    const std::string RpcSocketPath;
    rpc::TContainerRequest Req;
    rpc::TContainerResponse Rsp;
    int LastError;
    std::string LastErrorMsg;

    int SendReceive(int fd, rpc::TContainerRequest &req,
                    rpc::TContainerResponse &rsp);
    int Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp);

public:
    TPortoAPI(const std::string &path, int retries = 5);
    ~TPortoAPI();
    int Create(const std::string &name);
    int Destroy(const std::string &name);

    int Start(const std::string &name);
    int Stop(const std::string &name);
    int Kill(const std::string &name, int sig);
    int Pause(const std::string &name);
    int Resume(const std::string &name);

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
    void Cleanup();

    // VolumeAPI
    int CreateVolume(const std::string &path, const std::string &source,
                     const std::string &quota, const std::string &flags);
    int DestroyVolume(const std::string &path);
    int ListVolumes(std::vector<TVolumeDescription> &vlist);
};
