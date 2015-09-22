#include "libporto.hpp"
#include "protobuf.hpp"

extern "C" {
#include <unistd.h>
}

class TPortoAPIImpl {
private:
    int Fd;
    const int Retries;
    const int RetryDelayUs = 1000000;
    const std::string RpcSocketPath;

    friend TPortoAPI;
    int LastError;
    std::string LastErrorMsg;

    rpc::TContainerRequest Req;
    rpc::TContainerResponse Rsp;

public:
    TPortoAPIImpl(const std::string &path, int retries);
    ~TPortoAPIImpl();

    rpc::TContainerRequest& GetReq() { return Req; }
    rpc::TContainerResponse& GetResp() { return Rsp; }

    void Send(rpc::TContainerRequest &req);
    int Recv(rpc::TContainerResponse &rsp);
    int SendReceive(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp);

    int Rpc();
    void Cleanup();
};

TPortoAPIImpl::TPortoAPIImpl(const std::string &path, int retries) : Fd(-1),
                                                                     Retries(retries),
                                                                     RpcSocketPath(path) {
}

TPortoAPIImpl::~TPortoAPIImpl() {
    Cleanup();
}

void TPortoAPIImpl::Cleanup() {
    close(Fd);
    Fd = -1;
}

void TPortoAPIImpl::Send(rpc::TContainerRequest &req) {
    google::protobuf::io::FileOutputStream post(Fd);
    WriteDelimitedTo(req, &post);
    post.Flush();
}

int TPortoAPIImpl::Recv(rpc::TContainerResponse &rsp) {
    google::protobuf::io::FileInputStream pist(Fd);
    if (ReadDelimitedFrom(&pist, &rsp)) {
        LastErrorMsg = rsp.errormsg();
        LastError = (int)rsp.error();
        return LastError;
    } else {
        return -1;
    }
}

int TPortoAPIImpl::SendReceive(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
    Send(req);
    return Recv(rsp);
}

TPortoAPI::TPortoAPI(const std::string &path, int retries) :
    Impl(new TPortoAPIImpl(path, retries)) {
}

TPortoAPI::~TPortoAPI() {
    Impl = nullptr;
}

void TPortoAPI::Cleanup() {
    Impl->Cleanup();
}

int TPortoAPIImpl::Rpc() {
    int ret;
    int retries = Retries;
    LastErrorMsg = "";
    LastError = (int)EError::Unknown;

retry:
    if (Fd < 0) {
        TError error = ConnectToRpcServer(RpcSocketPath, Fd);
        if (error) {
            LastErrorMsg = error.GetMsg();
            LastError = INT_MAX;

            if (error.GetErrno() == EACCES || error.GetErrno() == ENOENT)
                goto exit;

            goto exit_or_retry;
        }
    }

    Rsp.Clear();
    ret = SendReceive(Req, Rsp);
    if (ret < 0) {
        Cleanup();

exit_or_retry:
        if (retries--) {
            usleep(RetryDelayUs);
            goto retry;
        }
    }

exit:
    Req.Clear();

    return LastError;
}

int TPortoAPI::Raw(const std::string &message, std::string &responce) {
    if (!google::protobuf::TextFormat::ParseFromString(message, &Impl->GetReq()) ||
        !Impl->GetReq().IsInitialized())
        return -1;

    int ret = Impl->Rpc();
    if (!ret)
        responce = Impl->GetResp().ShortDebugString();

    return ret;
}

int TPortoAPI::Create(const std::string &name) {
    Impl->GetReq().mutable_create()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Destroy(const std::string &name) {
    Impl->GetReq().mutable_destroy()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::List(std::vector<std::string> &clist) {
    Impl->GetReq().mutable_list();

    int ret = Impl->Rpc();
    if (!ret) {
        for (int i = 0; i < Impl->GetResp().list().name_size(); i++)
            clist.push_back(Impl->GetResp().list().name(i));
    }

    return ret;
}

int TPortoAPI::Plist(std::vector<TProperty> &plist) {
    Impl->GetReq().mutable_propertylist();

    int ret = Impl->Rpc();
    if (!ret) {
        auto list = Impl->GetResp().propertylist();

        for (int i = 0; i < list.list_size(); i++)
            plist.push_back(TProperty(list.list(i).name(),
                                      list.list(i).desc()));
    }

    return ret;
}

int TPortoAPI::Dlist(std::vector<TData> &dlist) {
    Impl->GetReq().mutable_datalist();

    int ret = Impl->Rpc();
    if (!ret) {
        auto list = Impl->GetResp().datalist();

        for (int i = 0; i < list.list_size(); i++)
            dlist.push_back(TData(list.list(i).name(),
                                  list.list(i).desc()));
    }

    return ret;
}

int TPortoAPI::Get(const std::vector<std::string> &name,
                   const std::vector<std::string> &variable,
                   std::map<std::string, std::map<std::string, TPortoGetResponse>> &result) {
    auto get = Impl->GetReq().mutable_get();

    for (auto n : name)
        get->add_name(n);
    for (auto v : variable)
        get->add_variable(v);

    int ret = Impl->Rpc();
    if (!ret) {
         for (int i = 0; i < Impl->GetResp().get().list_size(); i++) {
             auto entry = Impl->GetResp().get().list(i);
             auto name = entry.name();

             for (int j = 0; j < entry.keyval_size(); j++) {
                 auto keyval = entry.keyval(j);

                 TPortoGetResponse resp;
                 resp.Error = 0;
                 if (keyval.has_error())
                     resp.Error = keyval.error();
                 if (keyval.has_errormsg())
                     resp.ErrorMsg = keyval.errormsg();
                 if (keyval.has_value())
                     resp.Value = keyval.value();

                 result[name][keyval.variable()] = resp;
             }
         }
    }

    return ret;
}

int TPortoAPI::GetProperty(const std::string &name, const std::string &property,
                           std::string &value) {
    Impl->GetReq().mutable_getproperty()->set_name(name);
    Impl->GetReq().mutable_getproperty()->set_property(property);

    int ret = Impl->Rpc();
    if (!ret)
        value.assign(Impl->GetResp().getproperty().value());

    return ret;
}

int TPortoAPI::SetProperty(const std::string &name, const std::string &property,
                           std::string value) {
    Impl->GetReq().mutable_setproperty()->set_name(name);
    Impl->GetReq().mutable_setproperty()->set_property(property);
    Impl->GetReq().mutable_setproperty()->set_value(value);

    return Impl->Rpc();
}

int TPortoAPI::GetData(const std::string &name, const std::string &data,
                       std::string &value) {
    Impl->GetReq().mutable_getdata()->set_name(name);
    Impl->GetReq().mutable_getdata()->set_data(data);

    int ret = Impl->Rpc();
    if (!ret)
        value.assign(Impl->GetResp().getdata().value());

    return ret;
}

int TPortoAPI::GetVersion(std::string &tag, std::string &revision) {
    Impl->GetReq().mutable_version();

    int ret = Impl->Rpc();
    if (!ret) {
        tag = Impl->GetResp().version().tag();
        revision = Impl->GetResp().version().revision();
    }

    return ret;
}

int TPortoAPI::Start(const std::string &name) {
    Impl->GetReq().mutable_start()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Stop(const std::string &name) {
    Impl->GetReq().mutable_stop()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Kill(const std::string &name, int sig) {
    Impl->GetReq().mutable_kill()->set_name(name);
    Impl->GetReq().mutable_kill()->set_sig(sig);

    return Impl->Rpc();
}

int TPortoAPI::Pause(const std::string &name) {
    Impl->GetReq().mutable_pause()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Resume(const std::string &name) {
    Impl->GetReq().mutable_resume()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Wait(const std::vector<std::string> &containers,
                    std::string &name, int timeout) {
    auto wait = Impl->GetReq().mutable_wait();

    for (auto &c : containers)
        wait->add_name(c);

    if (timeout >= 0)
        wait->set_timeout(timeout);

    int ret = Impl->Rpc();
    name.assign(Impl->GetResp().wait().name());
    return ret;
}

void TPortoAPI::GetLastError(int &error, std::string &msg) const {
    error = Impl->LastError;
    msg = Impl->LastErrorMsg;
}

int TPortoAPI::ListVolumeProperties(std::vector<TProperty> &properties) {
    Impl->GetReq().mutable_listvolumeproperties();
    int ret = Impl->Rpc();
    if (!ret) {
        for (auto prop: Impl->GetResp().volumepropertylist().properties())
            properties.push_back(TProperty(prop.name(), prop.desc()));
    }
    return ret;
}

int TPortoAPI::CreateVolume(const std::string &path,
                            const std::map<std::string, std::string> &config,
                            TVolumeDescription &result) {
    auto req = Impl->GetReq().mutable_createvolume();

    req->set_path(path);

    for (auto kv: config) {
        auto prop = req->add_properties();
        prop->set_name(kv.first);
        prop->set_value(kv.second);
    }

    int ret = Impl->Rpc();
    if (!ret) {
        auto volume = Impl->GetResp().volume();
        result.Path = volume.path();
        result.Containers = std::vector<std::string>(std::begin(volume.containers()), std::end(volume.containers()));
        result.Properties.clear();
        for (auto p: volume.properties())
            result.Properties[p.name()] = p.value();
    }
    return ret;
}

int TPortoAPI::CreateVolume(std::string &path,
                            const std::map<std::string, std::string> &config) {
    TVolumeDescription result;
    int ret = CreateVolume(path, config, result);
    if (!ret && path == "")
        path = result.Path;
    return ret;
}

int TPortoAPI::LinkVolume(const std::string &path, const std::string &container) {
    auto req = Impl->GetReq().mutable_linkvolume();

    req->set_path(path);
    if (container != "")
        req->set_container(container);

    return Impl->Rpc();
}

int TPortoAPI::UnlinkVolume(const std::string &path, const std::string &container) {
    auto req = Impl->GetReq().mutable_unlinkvolume();

    req->set_path(path);
    if (container != "")
        req->set_container(container);

    return Impl->Rpc();
}

int TPortoAPI::ListVolumes(const std::string &path,
                           const std::string &container,
                           std::vector<TVolumeDescription> &volumes) {
    auto req = Impl->GetReq().mutable_listvolumes();

    if (path != "")
        req->set_path(path);

    if (container != "")
        req->set_container(container);

    int ret = Impl->Rpc();
    if (!ret) {
        auto list = Impl->GetResp().volumelist();

        for (auto v: list.volumes()) {
            std::map<std::string, std::string> properties;
            std::vector<std::string> containers(v.containers().begin(), v.containers().end());

            for (auto p: v.properties())
                properties[p.name()] = p.value();

            volumes.push_back(TVolumeDescription(v.path(), properties, containers));
        }
    }

    return ret;
}

int TPortoAPI::ImportLayer(const std::string &layer,
                           const std::string &tarball, bool merge) {
    auto req = Impl->GetReq().mutable_importlayer();

    req->set_layer(layer);
    req->set_tarball(tarball);
    req->set_merge(merge);
    return Impl->Rpc();
}

int TPortoAPI::ExportLayer(const std::string &volume,
                           const std::string &tarball) {
    auto req = Impl->GetReq().mutable_exportlayer();

    req->set_volume(volume);
    req->set_tarball(tarball);
    return Impl->Rpc();
}

int TPortoAPI::RemoveLayer(const std::string &layer) {
    auto req = Impl->GetReq().mutable_removelayer();

    req->set_layer(layer);
    return Impl->Rpc();
}

int TPortoAPI::ListLayers(std::vector<std::string> &layers) {
    Impl->GetReq().mutable_listlayers();

    int ret = Impl->Rpc();
    if (!ret) {
        auto list = Impl->GetResp().layers().layer();
        layers.clear();
        layers.reserve(list.size());
        copy(list.begin(), list.end(), std::back_inserter(layers));
    }
    return ret;
}
