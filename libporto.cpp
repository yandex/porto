#include "porto.hpp"
#include "libporto.hpp"

using namespace std;

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

int TPortoAPI::SendReceive(int fd, rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
    google::protobuf::io::FileInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    WriteDelimitedTo(req, &post);
    post.Flush();

    if (ReadDelimitedFrom(&pist, &rsp))
        return (int)rsp.error();
    else
        return -1;
}

TPortoAPI::TPortoAPI() : fd(-1) {
}

TPortoAPI::~TPortoAPI() {
    close(fd);
}

int TPortoAPI::Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
    if (fd < 0) {
        TError error = ConnectToRpcServer(RPC_SOCK, fd);
        if (error)
            throw "Can't connect to RPC server: " + error.GetMsg();
    }

    rsp.Clear();
    int ret = SendReceive(fd, req, rsp);
    req.Clear();
    return ret;
}

int TPortoAPI::Raw(const std::string &message, string &responce) {
    if (!google::protobuf::TextFormat::ParseFromString(message, &req) ||
        !req.IsInitialized())
        return -1;

    int ret = Rpc(req, rsp);
    if (!ret)
        responce = rsp.ShortDebugString();

    return ret;
}

int TPortoAPI::Create(const string &name) {
    req.mutable_create()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Destroy(const string &name) {
    req.mutable_destroy()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::List(vector<string> &clist) {
    req.mutable_list();

    int ret = Rpc(req, rsp);
    if (!ret) {
        for (int i = 0; i < rsp.list().name_size(); i++)
            clist.push_back(rsp.list().name(i));
    }

    return ret;
}

int TPortoAPI::Plist(vector<TProperty> &plist) {
    req.mutable_propertylist();

    int ret = Rpc(req, rsp);
    if (!ret) {
        auto list = rsp.propertylist();

        for (int i = 0; i < list.list_size(); i++)
            plist.push_back(TProperty(list.list(i).name(),
                                      list.list(i).desc()));
    }

    return ret;
}

int TPortoAPI::Dlist(vector<TData> &dlist) {
    req.mutable_datalist();

    int ret = Rpc(req, rsp);
    if (!ret) {
        auto list = rsp.datalist();

        for (int i = 0; i < list.list_size(); i++)
            dlist.push_back(TData(list.list(i).name(),
                                  list.list(i).desc()));
    }

    return ret;
}

int TPortoAPI::GetProperty(const string &name, const string &property, string &value) {
    req.mutable_getproperty()->set_name(name);
    req.mutable_getproperty()->set_property(property);

    int ret = Rpc(req, rsp);
    if (!ret)
        value.assign(rsp.getproperty().value());

    return ret;
}

int TPortoAPI::SetProperty(const string &name, const string &property, string value) {
    req.mutable_setproperty()->set_name(name);
    req.mutable_setproperty()->set_property(property);
    req.mutable_setproperty()->set_value(value);

    return Rpc(req, rsp);
}

int TPortoAPI::GetData(const string &name, const string &data, string &value) {
    req.mutable_getdata()->set_name(name);
    req.mutable_getdata()->set_data(data);

    int ret = Rpc(req, rsp);
    if (!ret)
        value.assign(rsp.getdata().value());

    return ret;
}

int TPortoAPI::Start(const string &name) {
    req.mutable_start()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Stop(const string &name) {
    req.mutable_stop()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Pause(const string &name) {
    req.mutable_pause()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Resume(const string &name) {
    req.mutable_resume()->set_name(name);

    return Rpc(req, rsp);
}
