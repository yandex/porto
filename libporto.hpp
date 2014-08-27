#ifndef __LIBPORTO_HPP__
#define __LIBPORTO_HPP__

#include "porto.hpp"
#include "rpc.hpp"
#include "util/protobuf.hpp"

struct TProperty {
    std::string name;
    std::string description;

    TProperty(std::string name, std::string description) :
        name(name), description(description) {}
};

struct TData {
    std::string name;
    std::string description;

    TData(std::string name, std::string description) :
        name(name), description(description) {}
};

struct TValue {
    int error; // change to enum
    std::string value;
};

class TPortoAPI {
    int fd;
    rpc::TContainerRequest req;
    rpc::TContainerResponse rsp;
    int LastError;
    std::string LastErrorMsg;

    int SendReceive(int fd, rpc::TContainerRequest &req,
                    rpc::TContainerResponse &rsp);
    int Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp);

public:
    TPortoAPI();
    ~TPortoAPI();
    int Create(const std::string &name);
    int Destroy(const std::string &name);

    int Start(const std::string &name);
    int Stop(const std::string &name);
    int Pause(const std::string &name);
    int Resume(const std::string &name);

    int List(std::vector<std::string> &clist);
    int Plist(std::vector<TProperty> &plist);
    int Dlist(std::vector<TData> &dlist);

    int GetProperty(const std::string &name, const std::string &property, std::string &value);
    int SetProperty(const std::string &name, const std::string &property, std::string value);

    int GetData(const std::string &name, const std::string &data, std::string &value);

    int Raw(const std::string &message, std::string &response);
    void GetLastError(int &error, std::string &msg) const;
};

#endif
