#include "libporto.hpp"

using namespace std;

extern "C" {
#include <unistd.h>
}

int TPortoAPI::SendReceive(int fd, rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
    google::protobuf::io::FileInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    WriteDelimitedTo(req, &post);
    post.Flush();

    if (ReadDelimitedFrom(&pist, &rsp)) {
        LastErrorMsg = rsp.errormsg();
        LastError = (int)rsp.error();
        return LastError;
    } else {
        return -1;
    }
}

TPortoAPI::TPortoAPI() : Fd(-1) {
}

TPortoAPI::~TPortoAPI() {
    Cleanup();
}

int TPortoAPI::Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
    LastErrorMsg = "";
    LastError = (int)EError::Unknown;

    if (Fd < 0) {
        TError error = ConnectToRpcServer(RPC_SOCK, Fd);
        if (error) {
            LastErrorMsg = error.GetMsg();
            LastError = INT_MIN;
            return INT_MIN;
        }
    }

    rsp.Clear();
    int ret = SendReceive(Fd, req, rsp);
    req.Clear();
    if (ret < 0)
        Cleanup();
    return LastError;
}

int TPortoAPI::Raw(const std::string &message, string &responce) {
    if (!google::protobuf::TextFormat::ParseFromString(message, &Req) ||
        !Req.IsInitialized())
        return -1;

    int ret = Rpc(Req, Rsp);
    if (!ret)
        responce = Rsp.ShortDebugString();

    return ret;
}

int TPortoAPI::Create(const string &name) {
    Req.mutable_create()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Destroy(const string &name) {
    Req.mutable_destroy()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::List(vector<string> &clist) {
    Req.mutable_list();

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        for (int i = 0; i < Rsp.list().name_size(); i++)
            clist.push_back(Rsp.list().name(i));
    }

    return ret;
}

int TPortoAPI::Plist(vector<TProperty> &plist) {
    Req.mutable_propertylist();

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        auto list = Rsp.propertylist();

        for (int i = 0; i < list.list_size(); i++)
            plist.push_back(TProperty(list.list(i).name(),
                                      list.list(i).desc()));
    }

    return ret;
}

int TPortoAPI::Dlist(vector<TData> &dlist) {
    Req.mutable_datalist();

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        auto list = Rsp.datalist();

        for (int i = 0; i < list.list_size(); i++)
            dlist.push_back(TData(list.list(i).name(),
                                  list.list(i).desc()));
    }

    return ret;
}

int TPortoAPI::GetProperty(const string &name, const string &property, string &value) {
    Req.mutable_getproperty()->set_name(name);
    Req.mutable_getproperty()->set_property(property);

    int ret = Rpc(Req, Rsp);
    if (!ret)
        value.assign(Rsp.getproperty().value());

    return ret;
}

int TPortoAPI::SetProperty(const string &name, const string &property, string value) {
    Req.mutable_setproperty()->set_name(name);
    Req.mutable_setproperty()->set_property(property);
    Req.mutable_setproperty()->set_value(value);

    return Rpc(Req, Rsp);
}

int TPortoAPI::GetData(const string &name, const string &data, string &value) {
    Req.mutable_getdata()->set_name(name);
    Req.mutable_getdata()->set_data(data);

    int ret = Rpc(Req, Rsp);
    if (!ret)
        value.assign(Rsp.getdata().value());

    return ret;
}

int TPortoAPI::Start(const string &name) {
    Req.mutable_start()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Stop(const string &name) {
    Req.mutable_stop()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Kill(const std::string &name, int sig) {
    Req.mutable_kill()->set_name(name);
    Req.mutable_kill()->set_sig(sig);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Pause(const string &name) {
    Req.mutable_pause()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Resume(const string &name) {
    Req.mutable_resume()->set_name(name);

    return Rpc(Req, Rsp);
}

void TPortoAPI::GetLastError(int &error, std::string &msg) const {
    error = LastError;
    msg = LastErrorMsg;
}

void TPortoAPI::Cleanup() {
    close(Fd);
    Fd = -1;
}
