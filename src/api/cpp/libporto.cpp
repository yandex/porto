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

namespace Porto {

static const char PortoSocket[] = "/run/portod.socket";

class Connection::ConnectionImpl {
public:
    int Fd = -1;
    int Timeout = 0;

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

    ConnectionImpl() { }

    ~ConnectionImpl() {
        Close();
    }

    int Connect();

    int SetTimeout(int direction, int timeout);

    void Close() {
        if (Fd >= 0)
            close(Fd);
        Fd = -1;
    }

    int Send();
    int Recv();
    int Rpc();
};

int Connection::ConnectionImpl::Connect()
{
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    if (Fd >= 0)
        Close();

    Fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Fd < 0)
        return Error(errno, "socket");

    if (Timeout && SetTimeout(3, Timeout))
        return LastError;

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));
    peer_addr.sun_family = AF_UNIX;
    strncpy(peer_addr.sun_path, PortoSocket, sizeof(PortoSocket) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(Fd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0)
        return Error(errno, "connect");

    return EError::Success;
}

int Connection::ConnectionImpl::SetTimeout(int direction, int timeout)
{
    struct timeval tv;

    tv.tv_sec = timeout;
    tv.tv_usec = 0;

    if ((direction & 1) && setsockopt(Fd, SOL_SOCKET,
                SO_SNDTIMEO, &tv, sizeof tv))
        return Error(errno, "set send timeout");

    if ((direction & 2) && setsockopt(Fd, SOL_SOCKET,
                SO_RCVTIMEO, &tv, sizeof tv))
        return Error(errno, "set recv timeout");

    return EError::Success;
}

int Connection::ConnectionImpl::Send() {
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

int Connection::ConnectionImpl::Recv() {
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

int Connection::ConnectionImpl::Rpc() {
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

Connection::Connection() : Impl(new ConnectionImpl()) { }

Connection::~Connection() {
    Impl = nullptr;
}

int Connection::Connect() {
    return Impl->Connect();
}

int Connection::SetTimeout(int timeout) {
    Impl->Timeout = timeout;
    if (Impl->Fd >= 0)
        return Impl->SetTimeout(3, timeout);
    return EError::Success;
}

void Connection::Close() {
    Impl->Close();
}

int Connection::Raw(const std::string &message, std::string &responce) {
    if (!google::protobuf::TextFormat::ParseFromString(message, &Impl->Req) ||
        !Impl->Req.IsInitialized())
        return -1;

    int ret = Impl->Rpc();
    if (!ret)
        responce = Impl->Rsp.ShortDebugString();

    return ret;
}

int Connection::Create(const std::string &name) {
    Impl->Req.mutable_create()->set_name(name);

    return Impl->Rpc();
}

int Connection::CreateWeakContainer(const std::string &name) {
    Impl->Req.mutable_createweak()->set_name(name);

    return Impl->Rpc();
}

int Connection::Destroy(const std::string &name) {
    Impl->Req.mutable_destroy()->set_name(name);

    return Impl->Rpc();
}

int Connection::List(std::vector<std::string> &list, const std::string &mask) {
    Impl->Req.mutable_list();

    if(!mask.empty())
        Impl->Req.mutable_list()->set_mask(mask);

    int ret = Impl->Rpc();
    if (!ret)
        list = std::vector<std::string>(std::begin(Impl->Rsp.list().name()),
                                        std::end(Impl->Rsp.list().name()));

    return ret;
}

int Connection::Plist(std::vector<Property> &list) {
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

int Connection::Dlist(std::vector<Property> &list) {
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

int Connection::Get(const std::vector<std::string> &name,
                   const std::vector<std::string> &variable,
                   std::map<std::string, std::map<std::string, GetResponse>> &result,
                   bool nonblock) {
    auto get = Impl->Req.mutable_get();

    for (const auto &n : name)
        get->add_name(n);
    for (const auto &v : variable)
        get->add_variable(v);
    if (nonblock)
        get->set_nonblock(nonblock);

    int ret = Impl->Rpc();
    if (!ret) {
         for (int i = 0; i < Impl->Rsp.get().list_size(); i++) {
             const auto &entry = Impl->Rsp.get().list(i);
             const auto &name = entry.name();

             for (int j = 0; j < entry.keyval_size(); j++) {
                 auto keyval = entry.keyval(j);

                 GetResponse resp;
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

int Connection::GetProperty(const std::string &name, const std::string &property,
                           std::string &value) {
    auto* get_property = Impl->Req.mutable_getproperty();
    get_property->set_name(name);
    get_property->set_property(property);

    int ret = Impl->Rpc();
    if (!ret)
        value.assign(Impl->Rsp.getproperty().value());

    return ret;
}

int Connection::SetProperty(const std::string &name, const std::string &property,
                           std::string value) {
    auto* set_property = Impl->Req.mutable_setproperty();
    set_property->set_name(name);
    set_property->set_property(property);
    set_property->set_value(value);

    return Impl->Rpc();
}

int Connection::GetData(const std::string &name, const std::string &data,
                       std::string &value) {
    auto* get_data = Impl->Req.mutable_getdata();
    get_data->set_name(name);
    get_data->set_data(data);

    int ret = Impl->Rpc();
    if (!ret)
        value.assign(Impl->Rsp.getdata().value());

    return ret;
}

int Connection::GetVersion(std::string &tag, std::string &revision) {
    Impl->Req.mutable_version();

    int ret = Impl->Rpc();
    if (!ret) {
        tag = Impl->Rsp.version().tag();
        revision = Impl->Rsp.version().revision();
    }

    return ret;
}

int Connection::Start(const std::string &name) {
    Impl->Req.mutable_start()->set_name(name);

    return Impl->Rpc();
}

int Connection::Stop(const std::string &name, int timeout) {
    auto stop = Impl->Req.mutable_stop();

    stop->set_name(name);
    if (timeout >= 0)
        stop->set_timeout_ms(timeout * 1000);

    return Impl->Rpc();
}

int Connection::Kill(const std::string &name, int sig) {
    Impl->Req.mutable_kill()->set_name(name);
    Impl->Req.mutable_kill()->set_sig(sig);

    return Impl->Rpc();
}

int Connection::Pause(const std::string &name) {
    Impl->Req.mutable_pause()->set_name(name);

    return Impl->Rpc();
}

int Connection::Resume(const std::string &name) {
    Impl->Req.mutable_resume()->set_name(name);

    return Impl->Rpc();
}

int Connection::WaitContainers(const std::vector<std::string> &containers,
                               std::string &name, int timeout) {
    auto wait = Impl->Req.mutable_wait();
    int ret, recv_timeout = 0;

    for (const auto &c : containers)
        wait->add_name(c);

    if (timeout >= 0) {
        wait->set_timeout(timeout * 1000);
        recv_timeout = timeout + (Impl->Timeout ?: timeout);
    }

    if (Impl->Fd < 0 && Connect())
        return Impl->LastError;

    if (timeout && Impl->SetTimeout(2, recv_timeout))
        return Impl->LastError;

    ret = Impl->Rpc();

    if (timeout && Impl->Fd >= 0)
        Impl->SetTimeout(2, Impl->Timeout);

    name.assign(Impl->Rsp.wait().name());
    return ret;
}

void Connection::GetLastError(int &error, std::string &msg) const {
    error = Impl->LastError;
    msg = Impl->LastErrorMsg;
}

int Connection::ListVolumeProperties(std::vector<Property> &list) {
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

int Connection::CreateVolume(const std::string &path,
                            const std::map<std::string, std::string> &config,
                            Volume &result) {
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

int Connection::CreateVolume(std::string &path,
                            const std::map<std::string, std::string> &config) {
    Volume result;
    int ret = CreateVolume(path, config, result);
    if (!ret && path.empty())
        path = result.Path;
    return ret;
}

int Connection::LinkVolume(const std::string &path, const std::string &container) {
    auto req = Impl->Req.mutable_linkvolume();

    req->set_path(path);
    if (!container.empty())
        req->set_container(container);

    return Impl->Rpc();
}

int Connection::UnlinkVolume(const std::string &path, const std::string &container) {
    auto req = Impl->Req.mutable_unlinkvolume();

    req->set_path(path);
    if (!container.empty())
        req->set_container(container);

    return Impl->Rpc();
}

int Connection::ListVolumes(const std::string &path,
                           const std::string &container,
                           std::vector<Volume> &volumes) {
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

int Connection::TuneVolume(const std::string &path,
                          const std::map<std::string, std::string> &config) {
    auto req = Impl->Req.mutable_tunevolume();

    req->set_path(path);

    for (const auto &kv: config) {
        auto prop = req->add_properties();
        prop->set_name(kv.first);
        prop->set_value(kv.second);
    }

    return Impl->Rpc();
}

int Connection::ImportLayer(const std::string &layer,
                            const std::string &tarball, bool merge,
                            const std::string &place) {
    auto req = Impl->Req.mutable_importlayer();

    req->set_layer(layer);
    req->set_tarball(tarball);
    req->set_merge(merge);
    if (place.size())
        req->set_place(place);
    return Impl->Rpc();
}

int Connection::ExportLayer(const std::string &volume,
                           const std::string &tarball) {
    auto req = Impl->Req.mutable_exportlayer();

    req->set_volume(volume);
    req->set_tarball(tarball);
    return Impl->Rpc();
}

int Connection::RemoveLayer(const std::string &layer, const std::string &place) {
    auto req = Impl->Req.mutable_removelayer();

    req->set_layer(layer);
    if (place.size())
        req->set_place(place);
    return Impl->Rpc();
}

int Connection::ListLayers(std::vector<std::string> &layers, const std::string &place) {
    auto req = Impl->Req.mutable_listlayers();
    if (place.size())
        req->set_place(place);
    int ret = Impl->Rpc();
    if (!ret) {
        const auto &list = Impl->Rsp.layers().layer();
        layers.assign(std::begin(list), std::end(list));
    }
    return ret;
}

int Connection::ConvertPath(const std::string &path, const std::string &src,
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

} /* namespace Porto */
