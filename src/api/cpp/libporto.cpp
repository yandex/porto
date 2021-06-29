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

static const int PORTOD_RELOAD_ERRNO = 6000;

class Connection::ConnectionImpl {
public:
    int Fd = -1;
    int Timeout = DEFAULT_TIMEOUT;
    int DiskTimeout = DISK_TIMEOUT;
    bool EnablePortodReloadError = false;

    rpc::TContainerRequest Req;
    rpc::TContainerResponse Rsp;

    std::vector<std::string> AsyncWaitContainers;
    int AsyncWaitTimeout = -1;
    std::function<void(AsyncWaitEvent &event)> AsyncWaitCallback;

    int LastError = 0;
    std::string LastErrorMsg;

    int Error(int err, const std::string &prefix) {
        LastErrorMsg = std::string(prefix + ": " + strerror(err));

        switch (err) {
        case ENOENT:
            LastError = EError::SocketUnavailable;
            break;
        case EAGAIN:
            LastError = EError::SocketTimeout;
            break;
        case EIO:
        case EPIPE:
            LastError = EError::SocketError;
            break;
        case PORTOD_RELOAD_ERRNO:
            LastErrorMsg = "connection closed by server";
            LastError = EError::PortodReloaded;
            break;
        default:
            LastError = EError::Unknown;
            break;
        }
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

    int Send(const rpc::TContainerRequest &req);
    int Recv(rpc::TContainerResponse &rsp, bool enablePortodReloadError = false);
    int Call(const rpc::TContainerRequest &req, rpc::TContainerResponse &rsp,
             int extra_timeout = -1);
    int Call(int extra_timeout = -1);
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

    if (Timeout > 0 && SetTimeout(3, Timeout))
        return LastError;

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));
    peer_addr.sun_family = AF_UNIX;
    memcpy(peer_addr.sun_path, PortoSocket, sizeof(PortoSocket) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(Fd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0)
        return Error(errno, "connect");

    /* restore async wait */
    if (!AsyncWaitContainers.empty()) {
        for (auto &name: AsyncWaitContainers)
            Req.mutable_asyncwait()->add_name(name);
        if (AsyncWaitTimeout >= 0)
            Req.mutable_asyncwait()->set_timeout_ms(AsyncWaitTimeout * 1000);
        return Call();
    }

    return EError::Success;
}

int Connection::ConnectionImpl::SetTimeout(int direction, int timeout)
{
    struct timeval tv;

    if (Fd < 0)
        return EError::Success;

    tv.tv_sec = timeout > 0 ? timeout : 0;
    tv.tv_usec = 0;

    if ((direction & 1) && setsockopt(Fd, SOL_SOCKET,
                SO_SNDTIMEO, &tv, sizeof tv))
        return Error(errno, "set send timeout");

    if ((direction & 2) && setsockopt(Fd, SOL_SOCKET,
                SO_RCVTIMEO, &tv, sizeof tv))
        return Error(errno, "set recv timeout");

    return EError::Success;
}

int Connection::ConnectionImpl::Send(const rpc::TContainerRequest &req) {
    google::protobuf::io::FileOutputStream raw(Fd);

    {
        google::protobuf::io::CodedOutputStream output(&raw);

        output.WriteVarint32(req.ByteSize());
        req.SerializeWithCachedSizes(&output);
    }

    raw.Flush();

    int err = raw.GetErrno();
    if (err)
        return Error(err, "send");

    return EError::Success;
}

int Connection::ConnectionImpl::Recv(rpc::TContainerResponse &rsp, bool enablePortodReloadError) {
    google::protobuf::io::FileInputStream raw(Fd);
    google::protobuf::io::CodedInputStream input(&raw);

    while (true) {
        uint32_t size;

        if (!input.ReadVarint32(&size))
            return Error(raw.GetErrno() ?: (enablePortodReloadError ? PORTOD_RELOAD_ERRNO : EIO), "recv");

        auto prev_limit = input.PushLimit(size);

        rsp.Clear();

        if (!rsp.ParseFromCodedStream(&input))
            return Error(raw.GetErrno() ?: (enablePortodReloadError ? PORTOD_RELOAD_ERRNO : EIO), "recv");

        input.PopLimit(prev_limit);

        if (rsp.has_asyncwait()) {
            if (AsyncWaitCallback) {
                AsyncWaitEvent event = {
                    (time_t)rsp.asyncwait().when(),
                    rsp.asyncwait().name(),
                    rsp.asyncwait().state(),
                    rsp.asyncwait().label(),
                    rsp.asyncwait().value(),
                };
                AsyncWaitCallback(event);
            }
        } else
            return EError::Success;
    }
}


int Connection::ConnectionImpl::Call(const rpc::TContainerRequest &req,
                                     rpc::TContainerResponse &rsp,
                                     int extra_timeout) {
    int ret = 0;

    if (Fd < 0)
        ret = Connect();

    if (!ret) {
        ret = Send(req);
        if (ret == EError::SocketError) {
            ret = Connect();
            if (!ret)
                ret = Send(req);
        }
    }

    if (!ret && extra_timeout && Timeout > 0)
        ret = SetTimeout(2, extra_timeout > 0 ? (extra_timeout + Timeout) : -1);

    if (!ret)
        ret = Recv(rsp, EnablePortodReloadError);

    if (extra_timeout && Timeout > 0)
        SetTimeout(2, Timeout);

    if (!ret) {
        ret = LastError = (int)rsp.error();
        LastErrorMsg = rsp.errormsg();
    }

    return ret;
}

int Connection::ConnectionImpl::Call(int extra_timeout) {
    int ret = Call(Req, Rsp, extra_timeout);
    Req.Clear();
    return ret;
}

Connection::Connection() : Impl(new ConnectionImpl()) { }

Connection::~Connection() {
    Impl = nullptr;
}

int Connection::Connect() {
    return Impl->Connect();
}

int Connection::GetTimeout() const {
    return Impl->Timeout;
}

int Connection::SetTimeout(int timeout) {
    Impl->Timeout = timeout >= 0 ? timeout : DEFAULT_TIMEOUT;
    return Impl->SetTimeout(3, Impl->Timeout);
}

int Connection::GetDiskTimeout() const {
    return Impl->DiskTimeout;
}

int Connection::SetDiskTimeout(int disk_timeout) {
    Impl->DiskTimeout = disk_timeout;
    return EError::Success;
}

void Connection::SetEnablePortodReloadError(bool value) {
    Impl->EnablePortodReloadError = value;
}

bool Connection::GetEnablePortodReloadError() const {
    return Impl->EnablePortodReloadError;
}

void Connection::Close() {
    Impl->Close();
}

uint64_t Connection::ResponseTimestamp() const {
    return Impl->Rsp.timestamp();
}

int Connection::Call(const rpc::TContainerRequest &req,
                     rpc::TContainerResponse &rsp,
                     int extra_timeout) {

    if (!req.IsInitialized()) {
        Impl->LastError = EError::InvalidMethod;
        Impl->LastErrorMsg = "Request is not initialized";
        return (int)EError::InvalidMethod;
    }

    return Impl->Call(req, rsp, extra_timeout);
}

int Connection::Call(const std::string &req,
                     std::string &rsp,
                     int extra_timeout) {

    if (!google::protobuf::TextFormat::ParseFromString(req, &Impl->Req)) {
        Impl->LastError = EError::InvalidMethod;
        Impl->LastErrorMsg = "Cannot parse request";
        return (int)EError::InvalidMethod;
    }

    int ret = Call(Impl->Req, Impl->Rsp, extra_timeout);
    Impl->Req.Clear();
    rsp = Impl->Rsp.DebugString();

    return ret;
}

int Connection::Create(const std::string &name) {
    Impl->Req.mutable_create()->set_name(name);

    return Impl->Call();
}

int Connection::CreateWeakContainer(const std::string &name) {
    Impl->Req.mutable_createweak()->set_name(name);

    return Impl->Call();
}

int Connection::Destroy(const std::string &name) {
    Impl->Req.mutable_destroy()->set_name(name);

    return Impl->Call();
}

int Connection::List(std::vector<std::string> &list, const std::string &mask) {
    Impl->Req.mutable_list();

    if(!mask.empty())
        Impl->Req.mutable_list()->set_mask(mask);

    int ret = Impl->Call();
    if (!ret)
        list = std::vector<std::string>(std::begin(Impl->Rsp.list().name()),
                                        std::end(Impl->Rsp.list().name()));

    return ret;
}

int Connection::ListProperties(std::vector<Property> &list) {
    Impl->Req.mutable_propertylist();

    int ret = Impl->Call();
    bool has_data = false;
    int i = 0;

    if (!ret) {
        list.resize(Impl->Rsp.propertylist().list_size());
        for (const auto &prop: Impl->Rsp.propertylist().list()) {
            list[i].Name = prop.name();
            list[i].Description = prop.desc();
            list[i].ReadOnly =  prop.read_only();
            list[i].Dynamic =  prop.dynamic();
            has_data |= list[i].ReadOnly;
            i++;
        }
    }

    if (!has_data) {
        Impl->Req.mutable_datalist();
        ret = Impl->Call();
        if (!ret) {
            list.resize(list.size() + Impl->Rsp.datalist().list_size());
            for (const auto &data: Impl->Rsp.datalist().list()) {
                list[i].Name = data.name();
                list[i++].Description = data.desc();
            }
        }
    }

    return ret;
}

int Connection::Get(const std::vector<std::string> &name,
                   const std::vector<std::string> &variable,
                   std::map<std::string, std::map<std::string, GetResponse>> &result,
                   int flags) {
    auto get = Impl->Req.mutable_get();

    for (const auto &n : name)
        get->add_name(n);
    for (const auto &v : variable)
        get->add_variable(v);

    if (flags & GetFlags::NonBlock)
        get->set_nonblock(true);
    if (flags & GetFlags::Sync)
        get->set_sync(true);
    if (flags & GetFlags::Real)
        get->set_real(true);

    int ret = Impl->Call();
    if (!ret) {
         for (int i = 0; i < Impl->Rsp.get().list_size(); i++) {
             const auto &entry = Impl->Rsp.get().list(i);

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

                 result[entry.name()][keyval.variable()] = resp;
             }
         }
    }

    return ret;
}

int Connection::GetProperty(const std::string &name, const std::string &property,
                           std::string &value, int flags) {
    auto* get_property = Impl->Req.mutable_getproperty();
    get_property->set_name(name);
    get_property->set_property(property);
    if (flags & GetFlags::Sync)
        get_property->set_sync(true);
    if (flags & GetFlags::Real)
        get_property->set_real(true);

    int ret = Impl->Call();
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

    return Impl->Call();
}

int Connection::IncLabel(const std::string &name, const std::string &label,
                         int64_t add, int64_t &result) {
    auto cmd = Impl->Req.mutable_inclabel();
    cmd->set_name(name);
    cmd->set_label(label);
    cmd->set_add(add);
    int ret = Impl->Call();
    if (Impl->Rsp.has_inclabel())
        result = Impl->Rsp.inclabel().result();
    return ret;
}

int Connection::GetVersion(std::string &tag, std::string &revision) {
    Impl->Req.mutable_version();

    int ret = Impl->Call();
    if (!ret) {
        tag = Impl->Rsp.version().tag();
        revision = Impl->Rsp.version().revision();
    }

    return ret;
}

int Connection::Start(const std::string &name) {
    Impl->Req.mutable_start()->set_name(name);

    return Impl->Call();
}

int Connection::Stop(const std::string &name, int timeout) {
    auto stop = Impl->Req.mutable_stop();

    stop->set_name(name);
    if (timeout >= 0)
        stop->set_timeout_ms(timeout * 1000);

    return Impl->Call(timeout > 0 ? timeout : 0);
}

int Connection::Kill(const std::string &name, int sig) {
    Impl->Req.mutable_kill()->set_name(name);
    Impl->Req.mutable_kill()->set_sig(sig);

    return Impl->Call();
}

int Connection::Pause(const std::string &name) {
    Impl->Req.mutable_pause()->set_name(name);

    return Impl->Call();
}

int Connection::Resume(const std::string &name) {
    Impl->Req.mutable_resume()->set_name(name);

    return Impl->Call();
}

int Connection::Respawn(const std::string &name) {
    Impl->Req.mutable_respawn()->set_name(name);
    return Impl->Call();
}

int Connection::WaitContainers(const std::vector<std::string> &containers,
                               const std::vector<std::string> &labels,
                               std::string &name, int timeout) {
    time_t deadline = timeout >= 0 ? time(nullptr) + timeout : 0;
    time_t last_retry = 0;
    int ret;

    while (1) {
        auto wait = Impl->Req.mutable_wait();

        for (const auto &c : containers)
            wait->add_name(c);

        for (auto &label: labels)
            wait->add_label(label);

        if (timeout >= 0)
            wait->set_timeout_ms(timeout * 1000);

        ret = Impl->Call(timeout);
        if (ret != EError::SocketError)
            break;

        time_t now = time(nullptr);
        if (timeout >= 0) {
            if (now > deadline)
                break;
            timeout = deadline - now;
        }

        if (last_retry == now)
            sleep(1);
        last_retry = now;
    }

    name = Impl->Rsp.has_wait() ? Impl->Rsp.wait().name() : "";

    return ret;
}

int Connection::AsyncWait(const std::vector<std::string> &containers,
                          const std::vector<std::string> &labels,
                          std::function<void(AsyncWaitEvent &event)> callback,
                          int timeout) {
    Impl->AsyncWaitContainers.clear();
    Impl->AsyncWaitTimeout = timeout;
    Impl->AsyncWaitCallback = callback;
    for (auto &name: containers)
        Impl->Req.mutable_asyncwait()->add_name(name);
    for (auto &label: labels)
        Impl->Req.mutable_asyncwait()->add_label(label);
    if (timeout >= 0)
        Impl->Req.mutable_asyncwait()->set_timeout_ms(timeout * 1000);
    int ret = Impl->Call();
    if (ret)
        Impl->AsyncWaitCallback = nullptr;
    else
        Impl->AsyncWaitContainers = containers;
    return ret;
}

int Connection::Recv() {
    return Impl->Recv(Impl->Rsp);
}

std::string Connection::GetLastError() const {
    return rpc::EError_Name((EError)Impl->LastError) + ":(" + Impl->LastErrorMsg + ")";
}

void Connection::GetLastError(int &error, std::string &msg) const {
    error = Impl->LastError;
    msg = Impl->LastErrorMsg;
}

int Connection::ListVolumeProperties(std::vector<Property> &list) {
    Impl->Req.mutable_listvolumeproperties();

    int ret = Impl->Call();
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

    int ret = Impl->Call(Impl->DiskTimeout);
    if (!ret) {
        const auto &volume = Impl->Rsp.volume();
        result.Path = volume.path();
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

int Connection::LinkVolume(const std::string &path, const std::string &container,
        const std::string &target, bool read_only, bool required) {
    auto req = (target == "" && !required) ? Impl->Req.mutable_linkvolume() :
                                             Impl->Req.mutable_linkvolumetarget();
    req->set_path(path);
    if (!container.empty())
        req->set_container(container);
    if (target != "")
        req->set_target(target);
    if (read_only)
        req->set_read_only(read_only);
    if (required)
        req->set_required(required);
    return Impl->Call();
}

int Connection::UnlinkVolume(const std::string &path,
                             const std::string &container,
                             const std::string &target,
                             bool strict) {
    auto req = (target == "***") ? Impl->Req.mutable_unlinkvolume() :
                                   Impl->Req.mutable_unlinkvolumetarget();

    req->set_path(path);
    if (!container.empty())
        req->set_container(container);
    if (target != "***")
        req->set_target(target);
    if (strict)
        req->set_strict(strict);

    return Impl->Call(Impl->DiskTimeout);
}

int Connection::ListVolumes(const std::string &path,
                           const std::string &container,
                           std::vector<Volume> &volumes) {
    auto req = Impl->Req.mutable_listvolumes();

    if (!path.empty())
        req->set_path(path);

    if (!container.empty())
        req->set_container(container);

    int ret = Impl->Call();
    if (!ret) {
        const auto &list = Impl->Rsp.volumelist();

        volumes.resize(list.volumes().size());
        int i = 0;

        for (const auto &v: list.volumes()) {
            volumes[i].Path = v.path();
            int l = 0;
            if (v.links().size()) {
                volumes[i].Links.resize(v.links().size());
                for (auto &link: v.links()) {
                    volumes[i].Links[l].Container = link.container();
                    volumes[i].Links[l].Target = link.target();
                    volumes[i].Links[l].ReadOnly = link.read_only();
                    volumes[i].Links[l].Required = link.required();
                    ++l;
                }
            } else {
                volumes[i].Links.resize(v.containers().size());
                for (auto &ct: v.containers())
                    volumes[i].Links[l++].Container = ct;
            }
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

    return Impl->Call(Impl->DiskTimeout);
}

int Connection::ImportLayer(const std::string &layer,
                            const std::string &tarball, bool merge,
                            const std::string &place,
                            const std::string &private_value,
                            bool verboseError) {
    auto req = Impl->Req.mutable_importlayer();

    req->set_layer(layer);
    req->set_tarball(tarball);
    req->set_merge(merge);
    req->set_verbose_error(verboseError);
    if (place.size())
        req->set_place(place);
    if (private_value.size())
        req->set_private_value(private_value);
    return Impl->Call(Impl->DiskTimeout);
}

int Connection::ExportLayer(const std::string &volume,
                           const std::string &tarball,
                           const std::string &compress) {
    auto req = Impl->Req.mutable_exportlayer();

    req->set_volume(volume);
    req->set_tarball(tarball);
    if (compress.size())
        req->set_compress(compress);
    return Impl->Call(Impl->DiskTimeout);
}

int Connection::RemoveLayer(const std::string &layer, const std::string &place) {
    auto req = Impl->Req.mutable_removelayer();

    req->set_layer(layer);
    if (place.size())
        req->set_place(place);
    return Impl->Call(Impl->DiskTimeout);
}

int Connection::ListLayers(std::vector<Layer> &layers,
                           const std::string &place,
                           const std::string &mask) {
    auto req = Impl->Req.mutable_listlayers();
    if (place.size())
        req->set_place(place);
    if (mask.size())
        req->set_mask(mask);
    int ret = Impl->Call();
    if (!ret) {
        if (Impl->Rsp.layers().layers().size()) {
            for (auto &layer: Impl->Rsp.layers().layers()) {
                Layer l;
                l.Name = layer.name();
                l.OwnerUser = layer.owner_user();
                l.OwnerGroup = layer.owner_group();
                l.PrivateValue = layer.private_value();
                l.LastUsage = layer.last_usage();
                layers.push_back(l);
            }
        } else {
            for (auto &layer: Impl->Rsp.layers().layer()) {
                Layer l;
                l.Name = layer;
                layers.push_back(l);
            }
        }
    }
    return ret;
}

int Connection::GetLayerPrivate(std::string &private_value,
                                const std::string &layer,
                                const std::string &place) {
    auto req = Impl->Req.mutable_getlayerprivate();
    req->set_layer(layer);
    if (place.size())
        req->set_place(place);
    int ret = Impl->Call();
    if (!ret) {
        private_value = Impl->Rsp.layer_private().private_value();
    }
    return ret;
}

int Connection::SetLayerPrivate(const std::string &private_value,
                                const std::string &layer,
                                const std::string &place) {
    auto req = Impl->Req.mutable_setlayerprivate();
    req->set_layer(layer);
    req->set_private_value(private_value);
    if (place.size())
        req->set_place(place);
    return Impl->Call();
}

const rpc::TStorageListResponse *Connection::ListStorage(const std::string &place, const std::string &mask) {
    auto req = Impl->Req.mutable_liststorage();
    if (place.size())
        req->set_place(place);
    if (mask.size())
        req->set_mask(mask);
    if (Impl->Call())
        return nullptr;
    return &Impl->Rsp.storagelist();
}

int Connection::RemoveStorage(const std::string &name,
                              const std::string &place) {
    auto req = Impl->Req.mutable_removestorage();

    req->set_name(name);
    if (place.size())
        req->set_place(place);
    return Impl->Call(Impl->DiskTimeout);
}

int Connection::ImportStorage(const std::string &name,
                              const std::string &archive,
                              const std::string &place,
                              const std::string &compression,
                              const std::string &private_value) {
    auto req = Impl->Req.mutable_importstorage();

    req->set_name(name);
    req->set_tarball(archive);
    if (place.size())
        req->set_place(place);
    if (compression.size())
        req->set_compress(compression);
    if (private_value.size())
        req->set_private_value(private_value);
    return Impl->Call(Impl->DiskTimeout);
}

int Connection::ExportStorage(const std::string &name,
                              const std::string &archive,
                              const std::string &place,
                              const std::string &compression) {
    auto req = Impl->Req.mutable_exportstorage();

    req->set_name(name);
    req->set_tarball(archive);
    if (place.size())
        req->set_place(place);
    if (compression.size())
        req->set_compress(compression);
    return Impl->Call(Impl->DiskTimeout);
}

int Connection::ConvertPath(const std::string &path, const std::string &src,
                           const std::string &dest, std::string &res) {
    auto req = Impl->Req.mutable_convertpath();
    req->set_path(path);
    req->set_source(src);
    req->set_destination(dest);

    auto ret = Impl->Call();
    if (!ret)
        res = Impl->Rsp.convertpath().path();
    return ret;
}

int Connection::AttachProcess(const std::string &name,
                              int pid, const std::string &comm) {
    auto req = Impl->Req.mutable_attachprocess();
    req->set_name(name);
    req->set_pid(pid);
    req->set_comm(comm);
    return Impl->Call();
}

int Connection::AttachThread(const std::string &name,
                              int pid, const std::string &comm) {
    auto req = Impl->Req.mutable_attachthread();
    req->set_name(name);
    req->set_pid(pid);
    req->set_comm(comm);
    return Impl->Call();
}

int Connection::LocateProcess(int pid, const std::string &comm,
                              std::string &name) {
    Impl->Req.mutable_locateprocess()->set_pid(pid);
    Impl->Req.mutable_locateprocess()->set_comm(comm);

    int ret = Impl->Call();

    if (ret)
        return ret;

    name = Impl->Rsp.locateprocess().name();

    return ret;
}

int Connection::GetContainerSpec(const std::string &name, rpc::TContainer &container) {
    rpc::TListContainersRequest req;
    auto filter = req.add_filters();
    filter->set_name(name);

    std::vector<rpc::TContainer> containers;

    int ret = ListContainersBy(req, containers);
    if (!ret && containers.size()) {
        container = containers[0];
        return EError::Success;
    }

    return ret;
}

int Connection::ListContainersBy(const rpc::TListContainersRequest &listContainersRequest, std::vector<rpc::TContainer> &containers) {
    auto req = Impl->Req.mutable_listcontainersby();
    *req = listContainersRequest;

    int ret = Impl->Call();
    if (ret)
        return ret;

    for (auto &ct : Impl->Rsp.listcontainersby().containers())
        containers.push_back(ct);

    return EError::Success;
}

int Connection::CreateFromSpec(const rpc::TContainerSpec &container, std::vector<rpc::TVolumeSpec> volumes, bool start) {
    auto req = Impl->Req.mutable_createfromspec();

    auto ct = req->mutable_container();
    *ct = container;

    for  (auto &volume : volumes) {
        auto v = req->add_volumes();
        *v = volume;
    }

    req->set_start(start);

    return Impl->Call();
}

int Connection::UpdateFromSpec(const rpc::TContainerSpec &container) {
    auto req = Impl->Req.mutable_updatefromspec();

    auto ct = req->mutable_container();
    *ct = container;

    return Impl->Call();
}

int Connection::ListVolumesBy(const rpc::TGetVolumeRequest &getVolumeRequest, std::vector<rpc::TVolumeSpec> &volumes) {
    auto req = Impl->Req.mutable_getvolume();
    *req = getVolumeRequest;

    int ret = Impl->Call();
    if (ret)
        return ret;

    for (auto volume : Impl->Rsp.getvolume().volume())
        volumes.push_back(volume);
    return EError::Success;
}

int Connection::CreateVolumeFromSpec(const rpc::TVolumeSpec &volume, rpc::TVolumeSpec &resultSpec) {
    auto req = Impl->Req.mutable_newvolume();
    auto vol = req->mutable_volume();
    *vol = volume;

    int ret = Impl->Call();
    if (ret)
        return ret;

    resultSpec =  Impl->Rsp.newvolume().volume();

    return ret;
}

} /* namespace Porto */
