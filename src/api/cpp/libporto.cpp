#include "libporto.hpp"

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>

#include "rpc.pb.h"

using ::rpc::EError;

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

static const char PortoSocket[] = "/run/portod.socket";

class TPortoAPI::TPortoAPIImpl {
public:
    int Fd = -1;

    rpc::TContainerRequest Req;
    rpc::TContainerResponse Rsp;

    int LastError;
    std::string LastErrorMsg;

    int Error(int err, const std::string &prefix) {
        LastError = EError::Unknown;
        LastErrorMsg = std::string(prefix + ": " + strerror(err));
        Close();
        return LastError;
    }

    TPortoAPIImpl() { }

    ~TPortoAPIImpl() {
        Close();
    }

    int Connect();

    void Close() {
        if (Fd >= 0)
            close(Fd);
        Fd = -1;
    }

    int Send();
    int Recv();
    int Rpc();
};

int TPortoAPI::TPortoAPIImpl::Connect()
{
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    if (Fd >= 0)
        Close();

    Fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Fd < 0)
        return Error(errno, "socket");

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));
    peer_addr.sun_family = AF_UNIX;
    strncpy(peer_addr.sun_path, PortoSocket, sizeof(PortoSocket) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(Fd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0)
        return Error(errno, "connect");

    return EError::Success;
}

int TPortoAPI::TPortoAPIImpl::Send() {
    google::protobuf::io::FileOutputStream raw(Fd);

    {
        google::protobuf::io::CodedOutputStream output(&raw);

        output.WriteVarint32(Req.ByteSize());
        Req.SerializeWithCachedSizes(&output);
    }

    raw.Flush();

    int err = raw.GetErrno();
    if (err)
        return Error(err, "send");

    return EError::Success;
}

int TPortoAPI::TPortoAPIImpl::Recv() {
    google::protobuf::io::FileInputStream raw(Fd);
    google::protobuf::io::CodedInputStream input(&raw);

    uint32_t size;
    if (input.ReadVarint32(&size)) {
        (void)input.PushLimit(size);
        if (Rsp.ParseFromCodedStream(&input))
            return EError::Success;
    }

    return Error(raw.GetErrno() ?: EIO, "recv");
}

int TPortoAPI::TPortoAPIImpl::Rpc() {
    int ret = 0;

    if (Fd < 0)
        ret = Connect();

    if (!ret)
        ret = Send();

    Req.Clear();

    if (!ret) {
        Rsp.Clear();
        ret = Recv();
    }

    if (!ret) {
        LastErrorMsg = Rsp.errormsg();
        LastError = (int)Rsp.error();
        ret = LastError;
    }

    return ret;
}

TPortoAPI::TPortoAPI() : Impl(new TPortoAPIImpl()) { }

TPortoAPI::~TPortoAPI() {
    Impl = nullptr;
}

int TPortoAPI::Connect() {
    return Impl->Connect();
}

void TPortoAPI::Close() {
    Impl->Close();
}

int TPortoAPI::Raw(const std::string &message, std::string &responce) {
    if (!google::protobuf::TextFormat::ParseFromString(message, &Impl->Req) ||
        !Impl->Req.IsInitialized())
        return -1;

    int ret = Impl->Rpc();
    if (!ret)
        responce = Impl->Rsp.ShortDebugString();

    return ret;
}

int TPortoAPI::Create(const std::string &name) {
    Impl->Req.mutable_create()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::CreateWeakContainer(const std::string &name) {
    Impl->Req.mutable_createweak()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Destroy(const std::string &name) {
    Impl->Req.mutable_destroy()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::List(std::vector<std::string> &clist) {
    Impl->Req.mutable_list();

    int ret = Impl->Rpc();
    if (!ret)
        clist = std::vector<std::string>(std::begin(Impl->Rsp.list().name()),
                                         std::end(Impl->Rsp.list().name()));

    return ret;
}

int TPortoAPI::Plist(std::vector<TPortoProperty> &list) {
    Impl->Req.mutable_propertylist();

    int ret = Impl->Rpc();
    if (!ret) {
        int i = 0;
        list.resize(Impl->Rsp.propertylist().list_size());
        for (const auto &prop: Impl->Rsp.propertylist().list()) {
            list[i].Name = prop.name();
            list[i++].Description = prop.desc();
        }
    }

    return ret;
}

int TPortoAPI::Dlist(std::vector<TPortoProperty> &list) {
    Impl->Req.mutable_datalist();

    int ret = Impl->Rpc();
    if (!ret) {
        int i = 0;
        list.resize(Impl->Rsp.datalist().list_size());
        for (const auto &data: Impl->Rsp.datalist().list()) {
            list[i].Name = data.name();
            list[i++].Description = data.desc();
        }
    }

    return ret;
}

int TPortoAPI::Get(const std::vector<std::string> &name,
                   const std::vector<std::string> &variable,
                   std::map<std::string, std::map<std::string,
                                    TPortoGetResponse>> &result) {
    auto get = Impl->Req.mutable_get();

    for (const auto &n : name)
        get->add_name(n);
    for (const auto &v : variable)
        get->add_variable(v);

    int ret = Impl->Rpc();
    if (!ret) {
         for (int i = 0; i < Impl->Rsp.get().list_size(); i++) {
             const auto &entry = Impl->Rsp.get().list(i);
             const auto &name = entry.name();

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
    auto* get_property = Impl->Req.mutable_getproperty();
    get_property->set_name(name);
    get_property->set_property(property);

    int ret = Impl->Rpc();
    if (!ret)
        value.assign(Impl->Rsp.getproperty().value());

    return ret;
}

int TPortoAPI::SetProperty(const std::string &name, const std::string &property,
                           std::string value) {
    auto* set_property = Impl->Req.mutable_setproperty();
    set_property->set_name(name);
    set_property->set_property(property);
    set_property->set_value(value);

    return Impl->Rpc();
}

int TPortoAPI::GetData(const std::string &name, const std::string &data,
                       std::string &value) {
    auto* get_data = Impl->Req.mutable_getdata();
    get_data->set_name(name);
    get_data->set_data(data);

    int ret = Impl->Rpc();
    if (!ret)
        value.assign(Impl->Rsp.getdata().value());

    return ret;
}

int TPortoAPI::GetVersion(std::string &tag, std::string &revision) {
    Impl->Req.mutable_version();

    int ret = Impl->Rpc();
    if (!ret) {
        tag = Impl->Rsp.version().tag();
        revision = Impl->Rsp.version().revision();
    }

    return ret;
}

int TPortoAPI::Start(const std::string &name) {
    Impl->Req.mutable_start()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Stop(const std::string &name) {
    Impl->Req.mutable_stop()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Kill(const std::string &name, int sig) {
    Impl->Req.mutable_kill()->set_name(name);
    Impl->Req.mutable_kill()->set_sig(sig);

    return Impl->Rpc();
}

int TPortoAPI::Pause(const std::string &name) {
    Impl->Req.mutable_pause()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Resume(const std::string &name) {
    Impl->Req.mutable_resume()->set_name(name);

    return Impl->Rpc();
}

int TPortoAPI::Wait(const std::vector<std::string> &containers,
                    std::string &name, int timeout) {
    auto wait = Impl->Req.mutable_wait();

    for (const auto &c : containers)
        wait->add_name(c);

    if (timeout >= 0)
        wait->set_timeout(timeout);

    int ret = Impl->Rpc();
    name.assign(Impl->Rsp.wait().name());
    return ret;
}

void TPortoAPI::GetLastError(int &error, std::string &msg) const {
    error = Impl->LastError;
    msg = Impl->LastErrorMsg;
}

int TPortoAPI::ListVolumeProperties(std::vector<TPortoProperty> &list) {
    Impl->Req.mutable_listvolumeproperties();

    int ret = Impl->Rpc();
    if (!ret) {
        int i = 0;
        list.resize(Impl->Rsp.volumepropertylist().properties_size());
        for (const auto &prop: Impl->Rsp.volumepropertylist().properties()) {
            list[i].Name = prop.name();
            list[i++].Description =  prop.desc();
        }
    }

    return ret;
}

int TPortoAPI::CreateVolume(const std::string &path,
                            const std::map<std::string, std::string> &config,
                            TPortoVolume &result) {
    auto req = Impl->Req.mutable_createvolume();

    req->set_path(path);

    for (const auto &kv: config) {
        auto prop = req->add_properties();
        prop->set_name(kv.first);
        prop->set_value(kv.second);
    }

    int ret = Impl->Rpc();
    if (!ret) {
        const auto &volume = Impl->Rsp.volume();
        result.Path = volume.path();
        result.Containers = std::vector<std::string>(std::begin(volume.containers()), std::end(volume.containers()));
        for (const auto &p: volume.properties())
            result.Properties[p.name()] = p.value();
    }
    return ret;
}

int TPortoAPI::CreateVolume(std::string &path,
                            const std::map<std::string, std::string> &config) {
    TPortoVolume result;
    int ret = CreateVolume(path, config, result);
    if (!ret && path.empty())
        path = result.Path;
    return ret;
}

int TPortoAPI::LinkVolume(const std::string &path, const std::string &container) {
    auto req = Impl->Req.mutable_linkvolume();

    req->set_path(path);
    if (!container.empty())
        req->set_container(container);

    return Impl->Rpc();
}

int TPortoAPI::UnlinkVolume(const std::string &path, const std::string &container) {
    auto req = Impl->Req.mutable_unlinkvolume();

    req->set_path(path);
    if (!container.empty())
        req->set_container(container);

    return Impl->Rpc();
}

int TPortoAPI::ListVolumes(const std::string &path,
                           const std::string &container,
                           std::vector<TPortoVolume> &volumes) {
    auto req = Impl->Req.mutable_listvolumes();

    if (!path.empty())
        req->set_path(path);

    if (!container.empty())
        req->set_container(container);

    int ret = Impl->Rpc();
    if (!ret) {
        const auto &list = Impl->Rsp.volumelist();

        volumes.resize(list.volumes().size());
        int i = 0;

        for (const auto &v: list.volumes()) {
            volumes[i].Path = v.path();
            volumes[i].Containers = std::vector<std::string>(
                    std::begin(v.containers()), std::end(v.containers()));
            for (const auto &p: v.properties())
                volumes[i].Properties[p.name()] = p.value();
            i++;
        }
    }

    return ret;
}

int TPortoAPI::ImportLayer(const std::string &layer,
                           const std::string &tarball, bool merge) {
    auto req = Impl->Req.mutable_importlayer();

    req->set_layer(layer);
    req->set_tarball(tarball);
    req->set_merge(merge);
    return Impl->Rpc();
}

int TPortoAPI::ExportLayer(const std::string &volume,
                           const std::string &tarball) {
    auto req = Impl->Req.mutable_exportlayer();

    req->set_volume(volume);
    req->set_tarball(tarball);
    return Impl->Rpc();
}

int TPortoAPI::RemoveLayer(const std::string &layer) {
    auto req = Impl->Req.mutable_removelayer();

    req->set_layer(layer);
    return Impl->Rpc();
}

int TPortoAPI::ListLayers(std::vector<std::string> &layers) {
    Impl->Req.mutable_listlayers();

    int ret = Impl->Rpc();
    if (!ret) {
        const auto &list = Impl->Rsp.layers().layer();
        layers.assign(std::begin(list), std::end(list));
    }
    return ret;
}

int TPortoAPI::ConvertPath(const std::string &path, const std::string &src,
                           const std::string &dest, std::string &res) {
    auto req = Impl->Req.mutable_convertpath();
    req->set_path(path);
    req->set_source(src);
    req->set_destination(dest);

    auto ret = Impl->Rpc();
    if (!ret)
        res = Impl->Rsp.convertpath().path();
    return ret;
}
