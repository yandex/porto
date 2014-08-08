#include "libporto.hpp"

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

TPortoAPI::TPortoAPI() {
    TError error = ConnectToRpcServer(RPC_SOCK_PATH, fd);
    if (error)
        throw "Can't connect to RPC server: " + error.GetMsg();
}

TPortoAPI::~TPortoAPI() {
    close(fd);
}

int TPortoAPI::Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
    rsp.Clear();
    int ret = SendReceive(fd, req, rsp);
    req.Clear();
    return ret;
}

int TPortoAPI::Raw(std::string message, string &responce) {
    if (!google::protobuf::TextFormat::ParseFromString(message, &req) ||
        !req.IsInitialized())
        return -1;

    int ret = Rpc(req, rsp);
    if (!ret)
        responce = rsp.ShortDebugString();

    return ret;
}

int TPortoAPI::Create(string name) {
    req.mutable_create()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Destroy(string name) {
    req.mutable_destroy()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::List(vector<string> &clist) {
    auto list = new ::rpc::TContainerListRequest();
    req.set_allocated_list(list);

    int ret = Rpc(req, rsp);
    if (!ret) {
        for (int i = 0; i < rsp.list().name_size(); i++)
            clist.push_back(rsp.list().name(i));
    }

    return ret;
}

int TPortoAPI::Plist(vector<TProperty> &plist) {
    auto *list = new ::rpc::TContainerPropertyListRequest();
    req.set_allocated_propertylist(list);

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
    auto *list = new ::rpc::TContainerDataListRequest();
    req.set_allocated_datalist(list);

    int ret = Rpc(req, rsp);
    if (!ret) {
        auto list = rsp.datalist();

        for (int i = 0; i < list.list_size(); i++)
            dlist.push_back(TData(list.list(i).name(),
                                  list.list(i).desc()));
    }

    return ret;
}

int TPortoAPI::GetProperties(string name, string property, vector<string> &value) {
    req.mutable_getproperty()->set_name(name);
    req.mutable_getproperty()->add_property(property);

    int ret = Rpc(req, rsp);
    if (!ret)
        for (int i = 0; i < rsp.getproperty().value_size(); i++)
            value.push_back(rsp.getproperty().value(i));

    return ret;
}

int TPortoAPI::SetProperty(string name, string property, string value) {
    req.mutable_setproperty()->set_name(name);
    req.mutable_setproperty()->set_property(property);
    req.mutable_setproperty()->set_value(value);

    return Rpc(req, rsp);
}

int TPortoAPI::GetData(string name, string data, vector<string> &value) {
    req.mutable_getdata()->set_name(name);
    req.mutable_getdata()->add_data(data);

    int ret = Rpc(req, rsp);
    if (!ret)
        for (int i = 0; i < rsp.getdata().value_size(); i++)
            value.push_back(rsp.getdata().value(i));

    return ret;
}

int TPortoAPI::Start(string name) {
    req.mutable_start()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Stop(string name) {
    req.mutable_stop()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Pause(string name) {
    req.mutable_pause()->set_name(name);

    return Rpc(req, rsp);
}

int TPortoAPI::Resume(string name) {
    req.mutable_resume()->set_name(name);

    return Rpc(req, rsp);
}
