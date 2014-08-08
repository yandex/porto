#ifndef __LIBPORTO_HPP__
#define __LIBPORTO_HPP__

#include "rpc.hpp"
#include "protobuf.hpp"

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

    static int SendReceive(int fd, rpc::TContainerRequest &req,
                           rpc::TContainerResponse &rsp);
    int Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp);

public:
    TPortoAPI();
    ~TPortoAPI();
    int Create(std::string name);
    int Destroy(std::string name);

    int Start(std::string name);
    int Stop(std::string name);
    int Pause(std::string name);
    int Resume(std::string name);

    int List(std::vector<std::string> &clist);
    int Plist(std::vector<TProperty> &plist);
    int Dlist(std::vector<TData> &dlist);

    int GetProperties(std::string name, std::string property, std::vector<std::string> &value);
    int SetProperty(std::string name, std::string property, std::string value);

    int GetData(std::string name, std::string data, std::vector<std::string> &value);

    int Raw(std::string message, std::string &response);
};

#endif
