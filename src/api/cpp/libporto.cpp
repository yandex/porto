#include "libporto.hpp"

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

namespace Porto {

Connection::~Connection() {
    Close();
}

EError Connection::SetError(const std::string &prefix, int _errno) {
    switch (_errno) {
        case ENOENT:
            LastError = EError::SocketUnavailable;
            break;
        case EAGAIN:
            LastError = EError::SocketTimeout;
            break;
        case EIO:
            LastError = EError::SocketError;
            break;
        default:
            LastError = EError::Unknown;
            break;
    }
    LastErrorMsg = prefix + ": " + strerror(_errno);
    Close();
    return LastError;
}

std::string Connection::GetLastError() const {
    return rpc::EError_Name(LastError) + ":(" + LastErrorMsg + ")";
}

EError Connection::Connect(const char *socket_path) {
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    Close();

    Fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Fd < 0)
        return SetError("socket", errno);

    if (Timeout > 0 && SetSocketTimeout(3, Timeout))
        return LastError;

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));
    peer_addr.sun_family = AF_UNIX;
    strncpy(peer_addr.sun_path, socket_path, strlen(socket_path));

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(Fd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0)
        return SetError("connect", errno);

    /* Restore async wait state */
    if (!AsyncWaitNames.empty()) {
        for (auto &name: AsyncWaitNames)
            Req.mutable_asyncwait()->add_name(name);
        for (auto &label: AsyncWaitLabels)
            Req.mutable_asyncwait()->add_label(label);
        if (AsyncWaitTimeout >= 0)
            Req.mutable_asyncwait()->set_timeout_ms(AsyncWaitTimeout * 1000);
        return Call();
    }

    return EError::Success;
}

void Connection::Close() {
    if (Fd >= 0)
        close(Fd);
    Fd = -1;
}

EError Connection::SetSocketTimeout(int direction, int timeout) {
    struct timeval tv;

    if (timeout < 0 || Fd < 0)
        return EError::Success;

    tv.tv_sec = timeout;
    tv.tv_usec = 0;

    if ((direction & 1) && setsockopt(Fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv))
        return SetError("setsockopt SO_SNDTIMEO", errno);

    if ((direction & 2) && setsockopt(Fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv))
        return SetError("setsockopt SO_RCVTIMEO", errno);

    return EError::Success;
}

EError Connection::SetTimeout(int timeout) {
    Timeout = timeout >= 0 ? timeout : DEFAULT_TIMEOUT;
    return SetSocketTimeout(3, Timeout);
}

EError Connection::SetDiskTimeout(int timeout) {
    DiskTimeout = timeout >= 0 ? timeout : DEFAULT_DISK_TIMEOUT;
    return EError::Success;
}

EError Connection::Send(const rpc::TContainerRequest &req) {
    google::protobuf::io::FileOutputStream raw(Fd);

    if (!req.IsInitialized()) {
        LastError = EError::InvalidMethod;
        LastErrorMsg = "Request is not initialized";
        return EError::InvalidMethod;
    }

    {
        google::protobuf::io::CodedOutputStream output(&raw);

        output.WriteVarint32(req.ByteSize());
        req.SerializeWithCachedSizes(&output);
    }

    raw.Flush();

    int err = raw.GetErrno();
    if (err)
        return SetError("send", err);

    return EError::Success;
}

EError Connection::Recv(rpc::TContainerResponse &rsp) {
    google::protobuf::io::FileInputStream raw(Fd);
    google::protobuf::io::CodedInputStream input(&raw);

    while (true) {
        uint32_t size;

        if (!input.ReadVarint32(&size))
            return SetError("recv", raw.GetErrno() ?: EIO);

        auto prev_limit = input.PushLimit(size);

        rsp.Clear();

        if (!rsp.ParseFromCodedStream(&input))
            return SetError("recv", raw.GetErrno() ?: EIO);

        input.PopLimit(prev_limit);

        if (rsp.has_asyncwait()) {
            if (AsyncWaitCallback)
                AsyncWaitCallback(rsp.asyncwait());
            continue;
        }

        return EError::Success;
    }
}

EError Connection::Call(const rpc::TContainerRequest &req,
                        rpc::TContainerResponse &rsp,
                        int extra_timeout) {
    EError err = EError::Success;

    if (Fd < 0)
        err = Connect();

    if (!err)
        err = Send(req);

    if (!err && extra_timeout >= 0 && Timeout)
        err = SetSocketTimeout(2, extra_timeout ? (extra_timeout + Timeout) : 0);

    if (!err)
        err = Recv(rsp);

    if (extra_timeout >= 0 && Timeout)
        SetSocketTimeout(2, Timeout);

    if (!err) {
        err = LastError = rsp.error();
        LastErrorMsg = rsp.errormsg();
    }

    return err;
}

EError Connection::Call(int extra_timeout) {
    Call(Req, Rsp, extra_timeout);
    return LastError;
}

EError Connection::Call(const std::string &req,
                        std::string &rsp,
                        int extra_timeout) {
    Req.Clear();
    if (!google::protobuf::TextFormat::ParseFromString(req, &Req)) {
        LastError = EError::InvalidMethod;
        LastErrorMsg = "Cannot parse request";
        return EError::InvalidMethod;
    }

    Call(Req, Rsp, extra_timeout);
    rsp = Rsp.DebugString();

    return LastError;
}

EError Connection::GetVersion(std::string &tag, std::string &revision) {
    Req.Clear();
    Req.mutable_version();

    if (!Call()) {
        tag = Rsp.version().tag();
        revision = Rsp.version().revision();
    }

    return LastError;
}

const rpc::TGetSystemResponse *Connection::GetSystem() {
    Req.Clear();
    Req.mutable_getsystem();
    if (!Call())
        return &Rsp.getsystem();
    return nullptr;
}

EError Connection::SetSystem(const std::string &key, const std::string &val) {
    std::string rsp;
    return Call("SetSystem {" + key + ":" + val + "}", rsp);
}

/* Container */

EError Connection::Create(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_create();
    req->set_name(name);
    return Call();
}

EError Connection::CreateWeakContainer(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_createweak();
    req->set_name(name);
    return Call();
}

EError Connection::Destroy(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_destroy();
    req->set_name(name);
    return Call();
}

const rpc::TContainerListResponse *Connection::List(const std::string &mask) {
    auto req = Req.mutable_list();

    if(!mask.empty())
        req->set_mask(mask);

    if (!Call())
        return &Rsp.list();

    return nullptr;
}

EError Connection::List(std::vector<std::string> &list, const std::string &mask) {
    Req.Clear();
    auto req = Req.mutable_list();
    if(!mask.empty())
        req->set_mask(mask);
    if (!Call())
        list = std::vector<std::string>(std::begin(Rsp.list().name()),
                                        std::end(Rsp.list().name()));
    return LastError;
}

const rpc::TContainerPropertyListResponse *Connection::ListProperties() {
    Req.Clear();
    Req.mutable_propertylist();

    if (Call())
        return nullptr;

    bool has_data = false;
    for (const auto &prop: Rsp.propertylist().list()) {
        if (prop.read_only()) {
            has_data = true;
            break;
        }
    }

    if (!has_data) {
        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_datalist();
        if (!Call(req, rsp)) {
            for (const auto &data: rsp.datalist().list()) {
                auto d = Rsp.mutable_propertylist()->add_list();
                d->set_name(data.name());
                d->set_desc(data.desc());
                d->set_read_only(true);
            }
        }
    }

    return &Rsp.propertylist();
}

EError Connection::ListProperties(std::vector<std::string> &properties) {
    properties.clear();
    auto rsp = ListProperties();
    if (rsp) {
        for (auto &prop: rsp->list())
            properties.push_back(prop.name());
    }
    return LastError;
}

const rpc::TContainerGetResponse *Connection::Get(const std::vector<std::string> &names,
                                                  const std::vector<std::string> &vars,
                                                  int flags) {
    Req.Clear();
    auto get = Req.mutable_get();

    for (const auto &n : names)
        get->add_name(n);

    for (const auto &v : vars)
        get->add_variable(v);

    if (flags & GET_NONBLOCK)
        get->set_nonblock(true);
    if (flags & GET_SYNC)
        get->set_sync(true);
    if (flags & GET_REAL)
        get->set_real(true);

    if (!Call())
        return &Rsp.get();

    return nullptr;
}

const rpc::TContainerSpec *Connection::GetContainerSpec(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_getcontainer();

    req->add_name(name);

    if (!Call() && Rsp.getcontainer().container().size())
        return &Rsp.getcontainer().container(0);

    return nullptr;
}

EError Connection::GetProperty(const std::string &name,
                               const std::string &property,
                               std::string &value,
                               int flags) {
    Req.Clear();
    auto req = Req.mutable_getproperty();

    req->set_name(name);
    req->set_property(property);
    if (flags & GET_SYNC)
        req->set_sync(true);
    if (flags & GET_REAL)
        req->set_real(true);

    if (!Call())
        value = Rsp.getproperty().value();

    return LastError;
}

EError Connection::GetProperty(const std::string &name,
                               const std::string &property,
                               uint64_t &value,
                               int flags) {
    std::string str;
    if (!GetProperty(name, property, str, flags)) {
        const char *ptr = str.c_str();
        char *end;

        errno = 0;
        value = strtoull(ptr, &end, 10);
        if (errno || end == ptr || *end) {
            LastError = EError::InvalidValue;
            LastErrorMsg = " value: " + str;
        }
    }
    return LastError;
}

EError Connection::SetProperty(const std::string &name,
                               const std::string &property,
                               const std::string &value) {
    Req.Clear();
    auto req = Req.mutable_setproperty();

    req->set_name(name);
    req->set_property(property);
    req->set_value(value);

    return Call();
}

EError Connection::IncLabel(const std::string &name,
                            const std::string &label,
                            int64_t add,
                            int64_t &result) {
    Req.Clear();
    auto req = Req.mutable_inclabel();

    req->set_name(name);
    req->set_label(label);
    req->set_add(add);

    Call();

    if (Rsp.has_inclabel())
        result = Rsp.inclabel().result();

    return LastError;
}
EError Connection::Start(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_start();

    req->set_name(name);

    return Call();
}

EError Connection::Stop(const std::string &name, int timeout) {
    Req.Clear();
    auto req = Req.mutable_stop();

    req->set_name(name);
    if (timeout >= 0)
        req->set_timeout_ms(timeout * 1000);

    return Call(timeout);
}

EError Connection::Kill(const std::string &name, int sig) {
    Req.Clear();
    auto req = Req.mutable_kill();

    req->set_name(name);
    req->set_sig(sig);

    return Call();
}

EError Connection::Pause(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_pause();

    req->set_name(name);

    return Call();
}

EError Connection::Resume(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_resume();

    req->set_name(name);

    return Call();
}

EError Connection::Respawn(const std::string &name) {
    Req.Clear();
    auto req = Req.mutable_respawn();

    req->set_name(name);

    return Call();
}

EError Connection::WaitContainer(const std::string &name,
                                 std::string &result_state,
                                 int wait_timeout) {
    Req.Clear();
    auto req = Req.mutable_wait();

    req->add_name(name);

    if (wait_timeout >= 0)
        req->set_timeout_ms(wait_timeout * 1000);

    if (!Call(wait_timeout)) {
        if (Rsp.wait().has_state())
            result_state = Rsp.wait().state();
        else if (Rsp.wait().name() == "")
            result_state = "timeout";
        else
            result_state = "dead";
    }

    return LastError;
}

EError Connection::WaitContainers(const std::vector<std::string> &names,
                                  std::string &result_name,
                                  std::string &result_state,
                                  int timeout) {
    Req.Clear();
    auto req = Req.mutable_wait();
    int ret;

    for (auto &c : names)
        req->add_name(c);
    if (timeout >= 0)
        req->set_timeout_ms(timeout * 1000);

    if (!Call(timeout)) {
        if (Rsp.wait().has_state())
            result_state = Rsp.wait().state();
        else if (Rsp.wait().name() == "")
            result_state = "timeout";
        else
            result_state = "dead";
    }

    result_name = Rsp.wait().name();

    return LastError;
}

const rpc::TContainerWaitResponse *
Connection::Wait(const std::vector<std::string> &names,
                 const std::vector<std::string> &labels,
                 int timeout) {
    Req.Clear();
    auto req = Req.mutable_wait();
    int ret;

    for (auto &c : names)
        req->add_name(c);
    for (auto &label: labels)
        req->add_label(label);
    if (timeout >= 0)
        req->set_timeout_ms(timeout * 1000);

    Call(timeout);

    if (Rsp.has_wait())
        return &Rsp.wait();

    return nullptr;
}

EError Connection::AsyncWait(const std::vector<std::string> &names,
                             const std::vector<std::string> &labels,
                             TWaitCallback callback,
                             int timeout) {
    Req.Clear();
    auto req = Req.mutable_asyncwait();

    AsyncWaitNames.clear();
    AsyncWaitLabels.clear();
    AsyncWaitTimeout = timeout;
    AsyncWaitCallback = callback;

    for (auto &name: names)
        req->add_name(name);
    for (auto &label: labels)
        req->add_label(label);
    if (timeout >= 0)
        req->set_timeout_ms(timeout * 1000);

    if (Call()) {
        AsyncWaitCallback = nullptr;
    } else {
        AsyncWaitNames = names;
        AsyncWaitLabels = labels;
    }

    return LastError;
}

EError Connection::ConvertPath(const std::string &path,
                               const std::string &src,
                               const std::string &dest,
                               std::string &res) {
    Req.Clear();
    auto req = Req.mutable_convertpath();

    req->set_path(path);
    req->set_source(src);
    req->set_destination(dest);

    if (!Call())
        res = Rsp.convertpath().path();

    return LastError;
}

EError Connection::AttachProcess(const std::string &name, int pid,
                                 const std::string &comm) {
    Req.Clear();
    auto req = Req.mutable_attachprocess();

    req->set_name(name);
    req->set_pid(pid);
    req->set_comm(comm);

    return Call();
}

EError Connection::AttachThread(const std::string &name, int pid,
                                const std::string &comm) {
    Req.Clear();
    auto req = Req.mutable_attachthread();

    req->set_name(name);
    req->set_pid(pid);
    req->set_comm(comm);

    return Call();
}

EError Connection::LocateProcess(int pid, const std::string &comm,
                                 std::string &name) {
    Req.Clear();
    auto req = Req.mutable_locateprocess();

    req->set_pid(pid);
    req->set_comm(comm);

    if (!Call())
        name = Rsp.locateprocess().name();

    return LastError;
}

/* Volume */

const rpc::TVolumePropertyListResponse *Connection::ListVolumeProperties() {
    Req.Clear();
    Req.mutable_listvolumeproperties();

    if (!Call())
        return &Rsp.volumepropertylist();

    return nullptr;
}

EError Connection::ListVolumeProperties(std::vector<std::string> &properties) {
    properties.clear();
    auto rsp = ListVolumeProperties();
    if (rsp) {
        for (auto &prop: rsp->properties())
            properties.push_back(prop.name());
    }
    return LastError;
}

EError Connection::CreateVolume(std::string &path,
                                const std::map<std::string, std::string> &config) {
    Req.Clear();
    auto req = Req.mutable_createvolume();

    req->set_path(path);

    for (const auto &kv: config) {
        auto prop = req->add_properties();
        prop->set_name(kv.first);
        prop->set_value(kv.second);
    }

    if (!Call(DiskTimeout) && path.empty())
        path = Rsp.volume().path();

    return LastError;
}

EError Connection::TuneVolume(const std::string &path,
                              const std::map<std::string, std::string> &config) {
    Req.Clear();
    auto req = Req.mutable_tunevolume();

    req->set_path(path);

    for (const auto &kv: config) {
        auto prop = req->add_properties();
        prop->set_name(kv.first);
        prop->set_value(kv.second);
    }

    return Call(DiskTimeout);
}

EError Connection::LinkVolume(const std::string &path,
                              const std::string &container,
                              const std::string &target,
                              bool read_only,
                              bool required) {
    Req.Clear();
    auto req = (target.empty() && !required) ? Req.mutable_linkvolume() :
                                               Req.mutable_linkvolumetarget();

    req->set_path(path);
    if (!container.empty())
        req->set_container(container);
    if (target != "")
        req->set_target(target);
    if (read_only)
        req->set_read_only(read_only);
    if (required)
        req->set_required(required);

    return Call();
}

EError Connection::UnlinkVolume(const std::string &path,
                                const std::string &container,
                                const std::string &target,
                                bool strict) {
    Req.Clear();
    auto req = (target == "***") ? Req.mutable_unlinkvolume() :
                                   Req.mutable_unlinkvolumetarget();

    req->set_path(path);
    if (!container.empty())
        req->set_container(container);
    if (target != "***")
        req->set_target(target);
    if (strict)
        req->set_strict(strict);

    return Call(DiskTimeout);
}

const rpc::TVolumeListResponse *
Connection::ListVolumes(const std::string &path,
                        const std::string &container) {
    Req.Clear();
    auto req = Req.mutable_listvolumes();

    if (!path.empty())
        req->set_path(path);

    if (!container.empty())
        req->set_container(container);

    if (Call())
        return nullptr;

    auto list = Rsp.mutable_volumelist();

    /* compat */
    for (auto v: *list->mutable_volumes()) {
        if (v.links().size())
            break;
        for (auto &ct: v.containers())
            v.add_links()->set_container(ct);
    }

    return list;
}

EError Connection::ListVolumes(std::vector<std::string> &paths) {
    Req.Clear();
    auto rsp = ListVolumes();
    paths.clear();
    if (rsp) {
        for (auto &v : rsp->volumes())
            paths.push_back(v.path());
    }
    return LastError;
}

const rpc::TVolumeDescription *Connection::GetVolume(const std::string &path) {
    Req.Clear();
    auto rsp = ListVolumes(path);

    if (rsp && rsp->volumes().size())
        return &rsp->volumes(0);

    return nullptr;
}

const rpc::TVolumeSpec *Connection::GetVolumeSpec(const std::string &path) {
    Req.Clear();
    auto req = Req.mutable_getvolume();

    req->add_path(path);

    if (!Call() && Rsp.getvolume().volume().size())
        return &Rsp.getvolume().volume(0);

    return nullptr;
}

/* Layer*/

EError Connection::ImportLayer(const std::string &layer,
                               const std::string &tarball,
                               bool merge,
                               const std::string &place,
                               const std::string &private_value) {
    Req.Clear();
    auto req = Req.mutable_importlayer();

    req->set_layer(layer);
    req->set_tarball(tarball);
    req->set_merge(merge);
    if (place.size())
        req->set_place(place);
    if (private_value.size())
        req->set_private_value(private_value);

    return Call(DiskTimeout);
}

EError Connection::ExportLayer(const std::string &volume,
                               const std::string &tarball,
                               const std::string &compress) {
    Req.Clear();
    auto req = Req.mutable_exportlayer();

    req->set_volume(volume);
    req->set_tarball(tarball);
    if (compress.size())
        req->set_compress(compress);

    return Call(DiskTimeout);
}

EError Connection::ReExportLayer(const std::string &layer,
                                 const std::string &tarball,
                                 const std::string &compress) {
    Req.Clear();
    auto req = Req.mutable_exportlayer();

    req->set_volume("");
    req->set_layer(layer);
    req->set_tarball(tarball);
    if (compress.size())
        req->set_compress(compress);

    return Call(DiskTimeout);
}

EError Connection::RemoveLayer(const std::string &layer,
                               const std::string &place) {
    Req.Clear();
    auto req = Req.mutable_removelayer();

    req->set_layer(layer);
    if (place.size())
        req->set_place(place);

    return Call(DiskTimeout);
}

const rpc::TLayerListResponse *Connection::ListLayers(const std::string &place,
                                                      const std::string &mask) {
    Req.Clear();
    auto req = Req.mutable_listlayers();

    if (place.size())
        req->set_place(place);
    if (mask.size())
        req->set_mask(mask);

    if (Call())
        return nullptr;

    auto list = Rsp.mutable_layers();

    /* compat conversion */
    if (!list->layers().size() && list->layer().size()) {
        for (auto &name: list->layer()) {
            auto l = list->add_layers();
            l->set_name(name);
            l->set_owner_user("");
            l->set_owner_group("");
            l->set_last_usage(0);
            l->set_private_value("");
        }
    }

    return list;
}

EError Connection::ListLayers(std::vector<std::string> layers,
                              const std::string &place,
                              const std::string &mask) {
    Req.Clear();
    auto req = Req.mutable_listlayers();

    if (place.size())
        req->set_place(place);
    if (mask.size())
        req->set_mask(mask);

    if (!Call())
        layers = std::vector<std::string>(std::begin(Rsp.layers().layer()),
                                          std::end(Rsp.layers().layer()));

    return LastError;
}

EError Connection::GetLayerPrivate(std::string &private_value,
                                   const std::string &layer,
                                   const std::string &place) {
    Req.Clear();
    auto req = Req.mutable_getlayerprivate();

    req->set_layer(layer);
    if (place.size())
        req->set_place(place);

    if (!Call())
        private_value = Rsp.layer_private().private_value();

    return LastError;
}

EError Connection::SetLayerPrivate(const std::string &private_value,
                                   const std::string &layer,
                                   const std::string &place) {
    Req.Clear();
    auto req = Req.mutable_setlayerprivate();

    req->set_layer(layer);
    req->set_private_value(private_value);
    if (place.size())
        req->set_place(place);

    return Call();
}

/* Storage */

const rpc::TStorageListResponse *
Connection::ListStorages(const std::string &place,
                         const std::string &mask) {
    Req.Clear();
    auto req = Req.mutable_liststorage();

    if (place.size())
        req->set_place(place);
    if (mask.size())
        req->set_mask(mask);

    if (Call())
        return nullptr;

    return &Rsp.storagelist();
}

EError Connection::ListStorages(std::vector<std::string> &storages,
                                const std::string &place,
                                const std::string &mask) {
    Req.Clear();
    auto req = Req.mutable_listlayers();

    if (place.size())
        req->set_place(place);
    if (mask.size())
        req->set_mask(mask);

    if (!Call()) {
        storages.clear();
        for (auto &storage: Rsp.storagelist().storages())
            storages.push_back(storage.name());
    }

    return LastError;
}

EError Connection::RemoveStorage(const std::string &storage,
                                 const std::string &place) {
    Req.Clear();
    auto req = Req.mutable_removestorage();

    req->set_name(storage);
    if (place.size())
        req->set_place(place);

    return Call(DiskTimeout);
}

EError Connection::ImportStorage(const std::string &storage,
                                 const std::string &archive,
                                 const std::string &place,
                                 const std::string &compression,
                                 const std::string &private_value) {
    Req.Clear();
    auto req = Req.mutable_importstorage();

    req->set_name(storage);
    req->set_tarball(archive);
    if (place.size())
        req->set_place(place);
    if (compression.size())
        req->set_compress(compression);
    if (private_value.size())
        req->set_private_value(private_value);

    return Call(DiskTimeout);
}

EError Connection::ExportStorage(const std::string &storage,
                                 const std::string &archive,
                                 const std::string &place,
                                 const std::string &compression) {
    Req.Clear();
    auto req = Req.mutable_exportstorage();

    req->set_name(storage);
    req->set_tarball(archive);
    if (place.size())
        req->set_place(place);
    if (compression.size())
        req->set_compress(compression);

    return Call(DiskTimeout);
}

} /* namespace Porto */
