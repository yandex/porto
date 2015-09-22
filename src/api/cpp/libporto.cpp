#include "libporto.hpp"

extern "C" {
#include <unistd.h>
}

void TPortoAPI::Send(rpc::TContainerRequest &req) {
    google::protobuf::io::FileOutputStream post(Fd);
    WriteDelimitedTo(req, &post);
    post.Flush();
}

int TPortoAPI::Recv(rpc::TContainerResponse &rsp) {
    google::protobuf::io::FileInputStream pist(Fd);
    if (ReadDelimitedFrom(&pist, &rsp)) {
        LastErrorMsg = rsp.errormsg();
        LastError = (int)rsp.error();
        return LastError;
    } else {
        return -1;
    }
}

int TPortoAPI::SendReceive(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
    Send(req);
    return Recv(rsp);
}

TPortoAPI::TPortoAPI(const std::string &path, int retries) : Fd(-1), Retries(retries), RpcSocketPath(path) {
}

TPortoAPI::~TPortoAPI() {
    Cleanup();
}

int TPortoAPI::Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp) {
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

    rsp.Clear();
    ret = SendReceive(req, rsp);
    if (ret < 0) {
        Cleanup();

exit_or_retry:
        if (retries--) {
            usleep(RetryDelayUs);
            goto retry;
        }
    }

exit:
    req.Clear();

    return LastError;
}

int TPortoAPI::Raw(const std::string &message, std::string &responce) {
    if (!google::protobuf::TextFormat::ParseFromString(message, &Req) ||
        !Req.IsInitialized())
        return -1;

    int ret = Rpc(Req, Rsp);
    if (!ret)
        responce = Rsp.ShortDebugString();

    return ret;
}

int TPortoAPI::Create(const std::string &name) {
    Req.mutable_create()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Destroy(const std::string &name) {
    Req.mutable_destroy()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::List(std::vector<std::string> &clist) {
    Req.mutable_list();

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        for (int i = 0; i < Rsp.list().name_size(); i++)
            clist.push_back(Rsp.list().name(i));
    }

    return ret;
}

int TPortoAPI::Plist(std::vector<TProperty> &plist) {
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

int TPortoAPI::Dlist(std::vector<TData> &dlist) {
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

int TPortoAPI::Get(const std::vector<std::string> &name,
                   const std::vector<std::string> &variable,
                   std::map<std::string, std::map<std::string, TPortoGetResponse>> &result) {
    auto get = Req.mutable_get();

    for (auto n : name)
        get->add_name(n);
    for (auto v : variable)
        get->add_variable(v);

    int ret = Rpc(Req, Rsp);
    if (!ret) {
         for (int i = 0; i < Rsp.get().list_size(); i++) {
             auto entry = Rsp.get().list(i);
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
    Req.mutable_getproperty()->set_name(name);
    Req.mutable_getproperty()->set_property(property);

    int ret = Rpc(Req, Rsp);
    if (!ret)
        value.assign(Rsp.getproperty().value());

    return ret;
}

int TPortoAPI::SetProperty(const std::string &name, const std::string &property,
                           std::string value) {
    Req.mutable_setproperty()->set_name(name);
    Req.mutable_setproperty()->set_property(property);
    Req.mutable_setproperty()->set_value(value);

    return Rpc(Req, Rsp);
}

int TPortoAPI::GetData(const std::string &name, const std::string &data,
                       std::string &value) {
    Req.mutable_getdata()->set_name(name);
    Req.mutable_getdata()->set_data(data);

    int ret = Rpc(Req, Rsp);
    if (!ret)
        value.assign(Rsp.getdata().value());

    return ret;
}

int TPortoAPI::GetVersion(std::string &tag, std::string &revision) {
    Req.mutable_version();

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        tag = Rsp.version().tag();
        revision = Rsp.version().revision();
    }

    return ret;
}

int TPortoAPI::Start(const std::string &name) {
    Req.mutable_start()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Stop(const std::string &name) {
    Req.mutable_stop()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Kill(const std::string &name, int sig) {
    Req.mutable_kill()->set_name(name);
    Req.mutable_kill()->set_sig(sig);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Pause(const std::string &name) {
    Req.mutable_pause()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Resume(const std::string &name) {
    Req.mutable_resume()->set_name(name);

    return Rpc(Req, Rsp);
}

int TPortoAPI::Wait(const std::vector<std::string> &containers,
                    std::string &name, int timeout) {
    auto wait = Req.mutable_wait();

    for (auto &c : containers)
        wait->add_name(c);

    if (timeout >= 0)
        wait->set_timeout(timeout);

    int ret = Rpc(Req, Rsp);
    name.assign(Rsp.wait().name());
    return ret;
}

void TPortoAPI::GetLastError(int &error, std::string &msg) const {
    error = LastError;
    msg = LastErrorMsg;
}

void TPortoAPI::Cleanup() {
    close(Fd);
    Fd = -1;
}

int TPortoAPI::ListVolumeProperties(std::vector<TProperty> &properties) {
    Req.mutable_listvolumeproperties();
    int ret = Rpc(Req, Rsp);
    if (!ret) {
        for (auto prop: Rsp.volumepropertylist().properties())
            properties.push_back(TProperty(prop.name(), prop.desc()));
    }
    return ret;
}

int TPortoAPI::CreateVolume(const std::string &path,
                            const std::map<std::string, std::string> &config,
                            TVolumeDescription &result) {
    auto req = Req.mutable_createvolume();

    req->set_path(path);

    for (auto kv: config) {
        auto prop = req->add_properties();
        prop->set_name(kv.first);
        prop->set_value(kv.second);
    }

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        auto volume = Rsp.volume();
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
    auto req = Req.mutable_linkvolume();

    req->set_path(path);
    if (container != "")
        req->set_container(container);

    return Rpc(Req, Rsp);
}

int TPortoAPI::UnlinkVolume(const std::string &path, const std::string &container) {
    auto req = Req.mutable_unlinkvolume();

    req->set_path(path);
    if (container != "")
        req->set_container(container);

    return Rpc(Req, Rsp);
}

int TPortoAPI::ListVolumes(const std::string &path,
                           const std::string &container,
                           std::vector<TVolumeDescription> &volumes) {
    auto req = Req.mutable_listvolumes();

    if (path != "")
        req->set_path(path);

    if (container != "")
        req->set_container(container);

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        auto list = Rsp.volumelist();

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
    auto req = Req.mutable_importlayer();

    req->set_layer(layer);
    req->set_tarball(tarball);
    req->set_merge(merge);
    return Rpc(Req, Rsp);
}

int TPortoAPI::ExportLayer(const std::string &volume,
                           const std::string &tarball) {
    auto req = Req.mutable_exportlayer();

    req->set_volume(volume);
    req->set_tarball(tarball);
    return Rpc(Req, Rsp);
}

int TPortoAPI::RemoveLayer(const std::string &layer) {
    auto req = Req.mutable_removelayer();

    req->set_layer(layer);
    return Rpc(Req, Rsp);
}

int TPortoAPI::ListLayers(std::vector<std::string> &layers) {
    Req.mutable_listlayers();

    int ret = Rpc(Req, Rsp);
    if (!ret) {
        auto list = Rsp.layers().layer();
        layers.clear();
        layers.reserve(list.size());
        copy(list.begin(), list.end(), std::back_inserter(layers));
    }
    return ret;
}
