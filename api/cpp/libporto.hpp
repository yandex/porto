#ifndef __LIBPORTO_HPP__
#define __LIBPORTO_HPP__

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

    int GetProperty(const std::string &name, const std::string &property, std::string &value);
    int SetProperty(const std::string &name, const std::string &property, std::string value);

    int GetData(const std::string &name, const std::string &data, std::string &value);
    int GetVersion(std::string &tag, std::string &revision);

    int Raw(const std::string &message, std::string &response);
    void GetLastError(int &error, std::string &msg) const;
    void Cleanup();
};

#endif
