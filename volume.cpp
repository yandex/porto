#include <memory>
#include <sstream>
#include <algorithm>

#include "volume.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "util/unix.hpp"
#include "util/sha256.hpp"
#include "config.hpp"

extern "C" {
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include "util/ext4_proj_quota.h"
}

/* TVolumeBackend - abstract */

TError TVolumeBackend::Configure(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Clear() {
    return Volume->GetPath().ClearDirectory();
}

TError TVolumeBackend::Save(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Restore(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Resize(uint64_t space_limit, uint64_t inode_limit) {
    return TError(EError::NotSupported, "not implemented");
}

TError TVolumeBackend::Move(TPath) {
    return TError(EError::NotSupported, "not implemented");
}

TError TVolumeBackend::GetStat(uint64_t &space_used, uint64_t &space_avail,
                               uint64_t &inode_used, uint64_t &inode_avail) {
    return Volume->GetPath().StatVFS(space_used, space_avail,
                                     inode_used, inode_avail);
}

/* TVolumePlainBackend - bindmount */

class TVolumePlainBackend : public TVolumeBackend {
public:
    TVolumePlainBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) {}

    TError Build() override {
        TPath storage = Volume->GetStorage();

        TError error = storage.Chown(Volume->GetCred());
        if (error)
            return error;

        error = storage.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        TMount Mount = TMount(storage, Volume->GetPath(), "none", {});
        return Mount.Bind(Volume->IsReadOnly());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        auto storage = Volume->GetStorage();
        TMount Mount(storage, Volume->GetPath(), "none", {});
        TError error = Mount.Umount();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;

        return TError::Success();
    }

    TError Move(TPath dest) override {
        TMount mount(Volume->GetStorage(), Volume->GetPath(), "none", {});
        return mount.Move(dest);
    }
};

/* TVolumeNativeBackend - project quota + bindmount */

class TVolumeNativeBackend : public TVolumeBackend {
public:
    TVolumeNativeBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) {}

    TError Build() override {
        TPath storage = Volume->GetStorage();
        uint64_t space, inodes;

        Volume->GetQuota(space, inodes);
        if (ext4_create_project(storage.c_str(), space, inodes))
            return TError(EError::Unknown, errno, "ext4_create_project");

        TError error = storage.Chown(Volume->GetCred());
        if (error)
            return error;

        error = storage.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        TMount Mount = TMount(storage, Volume->GetPath(), "none", {});
        return Mount.Bind(Volume->IsReadOnly());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        auto storage = Volume->GetStorage();
        TMount Mount(storage, Volume->GetPath(), "none", {});
        TError error = Mount.Umount();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;

        if (ext4_destroy_project(storage.c_str()))
            L_ERR() << "Can't destroy ext4 project: " << errno << std::endl;

        return TError::Success();
    }

    TError Move(TPath dest) override {
        TMount mount(Volume->GetStorage(), Volume->GetPath(), "none", {});
        return mount.Move(dest);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        if (ext4_resize_project(Volume->GetStorage().c_str(),
                                space_limit, inode_limit))
            TError(EError::Unknown, errno, "ext4_resize_project");
        return TError::Success();
    }
};

/* TVolumeLoopBackend - ext4 image + loop device */

class TVolumeLoopBackend : public TVolumeBackend {
    int LoopDev = -1;

public:
    TVolumeLoopBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) { }

    TPath GetLoopImage() {
        return Volume->GetStorage().AddComponent("loop.img");
    }

    TPath GetLoopDevice() {
        if (LoopDev < 0)
            return TPath();
        return TPath("/dev/loop" + std::to_string(LoopDev));
    }

    TError Save(std::shared_ptr<TValueMap> Config) override {
        return Config->Set<int>(V_LOOP_DEV, LoopDev);
    }

    TError Restore(std::shared_ptr<TValueMap> Config) override {
        LoopDev = Config->Get<int>(V_LOOP_DEV);
        return TError::Success();
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        TPath image = GetLoopImage();
        uint64_t space_limit, inode_limit;
        TError error;

        Volume->GetQuota(space_limit, inode_limit);
        if (!space_limit)
            return TError(EError::InvalidValue, "loop backend requires space_limit");

        if (!image.Exists()) {
            L_ACT() << "Allocate loop image with size " << space_limit << std::endl;
            error = AllocLoop(image, space_limit);
            if (error)
                return error;
        } else {
            //FIXME call resize2fs
        }

        error = SetupLoopDevice(image, LoopDev);
        if (error)
            return error;

        TMount mount(GetLoopDevice(), path, "ext4", {});
        error = mount.Mount();
        if (error)
            goto free_loop;

        error = path.Chown(Volume->GetCred());
        if (error)
            goto umount_loop;

        error = path.Chmod(Volume->GetPermissions());
        if (error)
            goto umount_loop;

        return TError::Success();

umount_loop:
        (void)mount.Umount();
free_loop:
        PutLoopDev(LoopDev);
        LoopDev = -1;
        return error;
    }

    TError Destroy() override {
        TPath loop = GetLoopDevice();
        TPath path = Volume->GetPath();
        TError error;

        if (LoopDev < 0)
            return TError::Success();

        L_ACT() << "Destroy loop " << loop << std::endl;
        TMount mount(loop, path, "ext4", {});
        error = mount.Umount();
        TError error2 = PutLoopDev(LoopDev);
        if (!error)
            error = error2;
        LoopDev = -1;
        return error;
    }

    TError Clear() override {
        return Volume->GetPath().ClearDirectory();
    }

    TError Move(TPath dest) override {
        TMount mount(GetLoopDevice(), Volume->GetPath(), "ext4", {});
        return mount.Move(dest);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "loop backend doesn't suppport resize");
    }
};

/* TVolumeOverlayBackend - project quota + overlayfs */

class TVolumeOverlayBackend : public TVolumeBackend {
public:
    TVolumeOverlayBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) {}

    TError Build() override {
        TPath storage = Volume->GetStorage();
        TPath upper = storage.AddComponent("upper");
        TPath work = storage.AddComponent("work");
        uint64_t space_limit, inode_limit;
        TError error;
        std::stringstream lower;
        int index = 0;

        for (auto layer: Volume->GetLayers()) {
            if (index++)
                lower << ":";
            lower << layer;
        }

        TMount mount("overlay", Volume->GetPath(), "overlay",
                     { "lowerdir=" + lower.str(),
                       "upperdir=" + upper.ToString(),
                       "workdir=" + work.ToString() });

        Volume->GetQuota(space_limit, inode_limit);
        if (ext4_create_project(storage.c_str(),
                                space_limit, inode_limit))
            return TError(EError::Unknown, errno, "ext4_create_project");

        error = upper.Mkdir(0755);
        if (error)
            goto err;

        error = work.Mkdir(0755);
        if (error)
            goto err;

        error = upper.Chown(Volume->GetCred());
        if (error)
            goto err;

        error = upper.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        error = mount.Mount(Volume->IsReadOnly() ? MS_RDONLY : 0);
        if (!error)
            return error;
err:
        (void)ext4_destroy_project(storage.c_str());
        return error;
    }

    TError Clear() override {
        return Volume->GetStorage().AddComponent("upper").ClearDirectory();
    }

    TError Destroy() override {
        TPath storage = Volume->GetStorage();
        TMount mount("overlay", Volume->GetPath(), "overlay", {});
        TError error = mount.Umount();
        if (error)
            L_ERR() << "Can't umount overlay: " << error << std::endl;

        error = storage.ClearDirectory();
        if (error)
            L_ERR() << "Can't clear overlay storage: " << error << std::endl;

        if (ext4_destroy_project(storage.c_str()))
            L_ERR() << "Can't destroy ext4 project: " << errno << std::endl;

        return TError::Success();
    }

    TError Move(TPath dest) override {
        TMount mount("overlay", Volume->GetPath(), "overlay", {});
        return mount.Move(dest);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        if (ext4_resize_project(Volume->GetStorage().c_str(),
                                space_limit, inode_limit))
            TError(EError::Unknown, errno, "ext4_resize_project");
        return TError::Success();
    }
};

/* TVolume */

TError TVolume::OpenBackend() {
    if (GetBackend() == "plain")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumePlainBackend(shared_from_this()));
    else if (GetBackend() == "native")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeNativeBackend(shared_from_this()));
    else if (GetBackend() == "overlay")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeOverlayBackend(shared_from_this()));
    else if (GetBackend() == "loop")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeLoopBackend(shared_from_this()));
    else
        return TError(EError::InvalidValue, "Unknown volume backend: " + GetBackend());

    return TError::Success();
}

/* /place/porto_volumes/<id>/<type> */
TPath TVolume::GetInternal(std::string type) const {
    return TPath(config().volumes().volume_dir()).AddComponent(std::to_string(GetId())).AddComponent(type);
}

TPath TVolume::GetPath() const {
    auto val = Config->Find(V_PATH);
    if (val->HasValue())
        return val->Get<std::string>();
    else
        return GetInternal("volume");
}

TPath TVolume::GetStorage() const {
    auto val = Config->Find(V_STORAGE);
    if (val->HasValue())
        return val->Get<std::string>();
    else
        return GetInternal(GetBackend());
}

std::vector<TPath> TVolume::GetLayers() const {
    std::vector<TPath> result;

    for (auto layer: Config->Get<std::vector<std::string>>(V_LAYERS)) {
        TPath path(layer);
        if (!path.IsAbsolute())
            path = TPath(config().volumes().layers_dir()).AddComponent(layer);
        result.push_back(path);
    }

    return result;
}

TError TVolume::Configure(const TPath &path, const TCred &creator_cred,
                          std::shared_ptr<TContainer> creator_container,
                          const std::map<std::string, std::string> &properties) {
    TError error;

    /* Verify volume path */
    if (!path.IsEmpty()) {
        if (!path.IsAbsolute())
            return TError(EError::InvalidValue, "Volume path must be absolute");
        if (path.GetType() != EFileType::Directory)
            return TError(EError::InvalidValue, "Volume path must be a directory");
        if (!path.AccessOk(EFileAccess::Write, creator_cred))
            return TError(EError::Permission, "Volume path usage not permitted");
        error = Config->Set<std::string>(V_PATH, path.ToString());
        if (error)
            return error;
    }

    /* Verify storage path */
    if (properties.count(V_STORAGE)) {
        TPath storage(properties.at(V_STORAGE));
        if (!storage.IsAbsolute())
            return TError(EError::InvalidValue, "Storage path must be absolute");
        if (storage.GetType() != EFileType::Directory)
            return TError(EError::InvalidValue, "Storage path must be a directory");
        if (!storage.AccessOk(EFileAccess::Write, creator_cred))
            return TError(EError::Permission, "Storage path usage not permitted");
    }

    /* Save original creator. Just for the record. */
    error = Config->Set<std::string>(V_CREATOR, creator_container->GetName() + " " +
                    creator_cred.UserAsString() + " " + creator_cred.GroupAsString());
    if (error)
        return error;

    /* Set default credentials to creator */
    error = Config->Set<std::string>(V_USER, creator_cred.UserAsString());
    if (error)
        return error;
    error = Config->Set<std::string>(V_GROUP, creator_cred.GroupAsString());
    if (error)
        return error;

    /* Default permissions for volume root directory */
    error = Config->Set<std::string>(V_PERMISSIONS, "0775");
    if (error)
        return error;

    /* Apply properties */
    for (auto p: properties) {
        if (!Config->IsValid(p.first))
            return TError(EError::InvalidValue, "Invalid volume property: " + p.first);
        if (Config->IsReadOnly(p.first))
            return TError(EError::InvalidValue, "Read-only volume property: " + p.first);
        error = Config->FromString(p.first, p.second);
        if (error)
            return error;
    }

    error = Cred.Parse(Config->Get<std::string>(V_USER),
                       Config->Get<std::string>(V_GROUP));
    if (error)
        return error;

    /* Verify default credentials */
    if (Cred.Uid != creator_cred.Uid && !creator_cred.IsPrivileged())
        return TError(EError::Permission, "Changing user is not permitted");

    if (Cred.Gid != creator_cred.Gid && !creator_cred.IsPrivileged() &&
            !creator_cred.MemberOf(Cred.Gid))
        return TError(EError::Permission, "Changing group is not permitted");

    /* Verify default permissions */
    error = StringToOct(Config->Get<std::string>(V_PERMISSIONS), Permissions);
    if (error)
        return error;

    /* Autodetect volume backend */
    if (!Config->HasValue(V_BACKEND)) {
        if (Config->HasValue(V_LAYERS)) {
            if (!config().volumes().native()) //FIXME
                return TError(EError::InvalidValue, "overlay not supported");
            error = Config->Set<std::string>(V_BACKEND, "overlay");
        } else if (config().volumes().native())
            error = Config->Set<std::string>(V_BACKEND, "native");
        else if (Config->HasValue(V_SPACE_LIMIT) ||
                 Config->HasValue(V_INODE_LIMIT))
            error = Config->Set<std::string>(V_BACKEND, "loop");
        else
            error = Config->Set<std::string>(V_BACKEND, "plain");
        if (error)
            return error;
    }

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Configure(Config);
    if (error)
        return error;

    return TError::Success();
}

TError TVolume::Build() {
    TPath storage = GetStorage();
    TPath path = GetPath();
    TPath internal = GetInternal("");

    L_ACT() << "Build volume " << GetPath() << std::endl;

    TError error = internal.Mkdir(0755);
    if (error)
        goto err_internal;

    if (!Config->HasValue(V_STORAGE)) {
        error = storage.Mkdir(0755);
        if (error)
            goto err_storage;
    }

    if (!Config->HasValue(V_PATH)) {
        error = path.Mkdir(0755);
        if (error)
            goto err_path;
    }

    error = Backend->Build();
    if (error)
        goto err_build;

    error = Backend->Save(Config);
    if (error)
        goto err_save;

    return TError::Success();

err_save:
    (void)Backend->Destroy();
err_build:
    if (!Config->HasValue(V_PATH))
        (void)path.Rmdir();
err_path:
    if (!Config->HasValue(V_STORAGE))
        (void)storage.Rmdir();
err_storage:
    (void)internal.Rmdir();
err_internal:
    return error;
}

TError TVolume::Clear() {
    L_ACT() << "Clear volume " << GetPath() << std::endl;
    return Backend->Clear();
}

TError TVolume::Destroy() {
    TPath internal = GetInternal("");
    TPath storage = GetStorage();
    TPath path = GetPath();
    TError ret = TError::Success(), error;

    L_ACT() << "Destroy volume " << GetPath() << std::endl;

    if (Backend) {
        error = Backend->Destroy();
        if (error) {
            L_ERR() << "Can't destroy volume backend: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (!Config->HasValue(V_STORAGE) && storage.Exists()) {
        error = storage.ClearDirectory();
        if (error) {
            L_ERR() << "Can't clear storage: " << error << std::endl;
            if (!ret)
                ret = error;
        }

        error = storage.Rmdir();
        if (error) {
            L_ERR() << "Can't remove storage: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (!Config->HasValue(V_PATH) && path.Exists()) {
        error = path.Rmdir();
        if (error) {
            L_ERR() << "Can't remove volume path: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (internal.Exists()) {
        error = internal.ClearDirectory();
        if (error) {
            L_ERR() << "Can't clear internal: " << error << std::endl;
            if (!ret)
                ret = error;
        }

        error = internal.Rmdir();
        if (error) {
            L_ERR() << "Can't remove internal: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    return ret;
}

TError TVolume::GetStat(uint64_t &space_used, uint64_t &space_avail,
                        uint64_t &inode_used, uint64_t &inode_avail) const {
    return Backend->GetStat(space_used, space_avail, inode_used, inode_avail);
}

TError TVolume::Resize(uint64_t space_limit, uint64_t inode_limit) {
    L_ACT() << "Resize volume " << GetPath() << " to " << space_limit << " " << inode_limit << std::endl;
    TError error = Backend->Resize(space_limit, inode_limit);
    if (error)
        return error;
    Config->Set<uint64_t>(V_SPACE_LIMIT, space_limit);
    Config->Set<uint64_t>(V_INODE_LIMIT, inode_limit);
    return TError::Success();
}

TError TVolume::LinkContainer(std::string name) {
    std::vector<std::string> containers(Config->Get<std::vector<std::string>>(V_CONTAINERS));
    containers.push_back(name);
    return Config->Set<std::vector<std::string>>(V_CONTAINERS, containers);
}

bool TVolume::UnlinkContainer(std::string name) {
    auto containers(Config->Get<std::vector<std::string>>(V_CONTAINERS));
    containers.erase(std::remove(containers.begin(), containers.end(), name), containers.end());
    TError error = Config->Set<std::vector<std::string>>(V_CONTAINERS, containers);
    return containers.empty();
}

std::map<std::string, std::string> TVolume::GetProperties() {
    uint64_t space_used, space_avail, inode_used, inode_avail;
    std::map<std::string, std::string> ret;

    if (IsReady() && !GetStat(space_used, space_avail, inode_used, inode_avail)) {
        ret[V_SPACE_USED] = std::to_string(space_used);
        ret[V_INODE_USED] = std::to_string(inode_used);
        ret[V_SPACE_AVAILABLE] = std::to_string(space_avail);
        ret[V_INODE_AVAILABLE] = std::to_string(inode_avail);
    }

    for (auto name: Config->List()) {
        auto property = Config->Find(name);
        if (!(property->GetFlags() & HIDDEN_VALUE) && property->HasValue())
            ret[name] = property->ToString();
    }

    return ret;
}

TError TVolume::CheckPermission(const TCred &ucred) const {
    if (ucred.IsPrivileged())
        return TError::Success();

    if (Cred == ucred)
        return TError::Success();

    if (ucred.MemberOf(Cred.Gid))
        return TError::Success();

    return TError(EError::Permission, "Permission denied");
}

TError TVolume::Restore() {
    if (!IsReady())
        return TError(EError::VolumeNotReady, "Volume not ready");

    TError error = Cred.Parse(Config->Get<std::string>(V_USER),
                              Config->Get<std::string>(V_GROUP));
    if (error)
        return TError(EError::InvalidValue, "Bad volume " + GetPath().ToString() + " credentials: " +
                      Config->Get<std::string>(V_USER) + " " +
                      Config->Get<std::string>(V_GROUP));

    error = StringToOct(Config->Get<std::string>(V_PERMISSIONS), Permissions);
    if (error)
        return error;

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Restore(Config);
    if (error)
        return error;

    return TError::Success();
}

/* TVolumeHolder */

const std::vector<std::pair<std::string, std::string>> TVolumeHolder::ListProperties() {
    return {
        { V_BACKEND,     "plain|native|loop|overlay  default - autodetect" },
        { V_STORAGE,     "path to data storage" },
        { V_READY,       "true|false (readonly)" },
        { V_USER,        "user  default - creator" },
        { V_GROUP,       "group  default - creator" },
        { V_PERMISSIONS, "directory permissions  default - 0775" },
        { V_CREATOR,     "container user group" },
        { V_READ_ONLY,   "true|false  default - false" },
        { V_LAYERS,      "top-layer;...;bottom-layer  overlay layers" },
        { V_SPACE_LIMIT, " " },
        { V_INODE_LIMIT, " " },
        //{ V_SPACE_GUARANTEE, " " },
        //{ V_INODE_GUARANTEE, " " },
        { V_SPACE_USED, " " },
        { V_INODE_USED, " " },
        { V_SPACE_AVAILABLE, " " },
        { V_INODE_AVAILABLE, " " },
    };
}

static void RegisterVolumeProperties(std::shared_ptr<TRawValueMap> m) {
    m->Add(V_PATH, new TStringValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_STORAGE, new TStringValue(PERSISTENT_VALUE));

    m->Add(V_BACKEND, new TStringValue(PERSISTENT_VALUE));

    m->Add(V_USER, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_GROUP, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_PERMISSIONS, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_CREATOR, new TStringValue(READ_ONLY_VALUE | PERSISTENT_VALUE));

    m->Add(V_ID, new TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_READY, new TBoolValue(READ_ONLY_VALUE | PERSISTENT_VALUE));
    m->Add(V_CONTAINERS, new TListValue(HIDDEN_VALUE | PERSISTENT_VALUE));

    m->Add(V_LOOP_DEV, new TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_READ_ONLY, new TBoolValue(PERSISTENT_VALUE));
    m->Add(V_LAYERS, new TListValue(PERSISTENT_VALUE));

    m->Add(V_SPACE_LIMIT, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));
    m->Add(V_INODE_LIMIT, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));

    //m->Add(V_SPACE_GUARANTEE, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));
    //m->Add(V_INODE_GUARANTEE, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));
}

TError TVolumeHolder::Create(std::shared_ptr<TVolume> &volume) {
    uint16_t id;

    TError error = IdMap.Get(id);
    if (error)
        return error;
    auto node = Storage->GetNode(id);
    auto config = std::make_shared<TValueMap>(node);
    RegisterVolumeProperties(config);
    error = config->Set<int>(V_ID, id);
    if (error) {
        config->Remove();
        IdMap.Put(id);
        return error;
    }
    volume = std::make_shared<TVolume>(shared_from_this(), config);
    return TError::Success();
}

void TVolumeHolder::Remove(std::shared_ptr<TVolume> volume) {
    volume->Config->Remove();
    IdMap.Put(volume->GetId());
}

TError TVolumeHolder::RestoreFromStorage(std::shared_ptr<TContainerHolder> Cholder) {
    std::vector<std::shared_ptr<TKeyValueNode>> list;

    TPath volumes = config().volumes().volume_dir();
    if (!volumes.Exists() || volumes.GetType() != EFileType::Directory) {
        TFolder dir(config().volumes().volume_dir());
        (void)dir.Remove(true);
        TError error = dir.Create(0755, true);
        if (error)
            return error;
    }

    TPath layers = config().volumes().layers_dir();
    if (!layers.Exists() || layers.GetType() != EFileType::Directory) {
        TFolder dir(layers.ToString());
        (void)dir.Remove(true);
        TError error = layers.Mkdir(0755);
        if (error)
            return error;
    }

    TError error = Storage->ListNodes(list);
    if (error)
        return error;

    for (auto &node : list) {
        L_ACT() << "Restore volume " << node->GetName() << std::endl;

        auto config = std::make_shared<TValueMap>(node);
        RegisterVolumeProperties(config);
        error = config->Restore();
        if (error || !config->HasValue(V_ID) ||
                IdMap.GetAt(config->Get<int>(V_ID))) {
            L_WRN() << "Corrupted volume config " << node << " removed: " << error << std::endl;
            (void)config->Remove();
            continue;
        }

        auto volume = std::make_shared<TVolume>(shared_from_this(), config);
        error = volume->Restore();
        if (error) {
            L_WRN() << "Corrupted volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy();
            (void)Remove(volume);
            continue;
        }

        error = Register(volume);
        if (error) {
            L_WRN() << "Cannot register volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy();
            (void)Remove(volume);
            continue;
        }

        for (auto name: volume->GetContainers()) {
            std::shared_ptr<TContainer> container;
            if (!Cholder->Get(name, container)) {
                container->LinkVolume(volume);
            } else if (!volume->UnlinkContainer(name)) {
                (void)volume->Destroy();
                (void)Unregister(volume);
                (void)Remove(volume);
            }
        }

        L() << "Volume " << volume->GetPath() << " restored" << std::endl;
    }

    L_ACT() << "Remove stale volumes..." << std::endl;

    std::vector<std::string> subdirs;
    error = TFolder(volumes).Items(EFileType::Directory, subdirs);
    if (error)
        L_ERR() << "Cannot list " << volumes << std::endl;

    for (auto dir_name: subdirs) {
        bool used = false;
        for (auto v: Volumes) {
            if (std::to_string(v.second->GetId()) == dir_name) {
                used = true;
                break;
            }
        }
        if (used)
            continue;

        TPath dir = volumes.AddComponent(dir_name);
        TPath mnt = dir.AddComponent("volume");
        if (mnt.Exists()) {
            TMount mount(mnt, mnt, "", {});
            (void)mount.Umount();
        }
        error = dir.ClearDirectory();
        if (error)
            L_ERR() << "Cannot clear directory " << dir << std::endl;
        error = dir.Rmdir();
        if (error)
            L_ERR() << "Cannot remove directory " << dir << std::endl;
    }

    return TError::Success();
}

void TVolumeHolder::Destroy() {
    while (Volumes.begin() != Volumes.end()) {
        auto name = Volumes.begin()->first;
        auto volume = Volumes.begin()->second;
        TError error = volume->Destroy();
        if (error)
            L_ERR() << "Can't destroy volume " << name << ": " << error << std::endl;
        Unregister(volume);
        Remove(volume);
    }
}

TError TVolumeHolder::Register(std::shared_ptr<TVolume> volume) {
    if (Volumes.find(volume->GetPath()) == Volumes.end()) {
        Volumes[volume->GetPath()] = volume;
        return TError::Success();
    }

    return TError(EError::VolumeAlreadyExists, "Volume already exists");
}

void TVolumeHolder::Unregister(std::shared_ptr<TVolume> volume) {
    Volumes.erase(volume->GetPath());
}

std::shared_ptr<TVolume> TVolumeHolder::Find(const TPath &path) {
    auto v = Volumes.find(path);
    if (v != Volumes.end())
        return v->second;
    else
        return nullptr;
}

std::vector<TPath> TVolumeHolder::ListPaths() const {
    std::vector<TPath> ret;

    for (auto v : Volumes)
        ret.push_back(v.first);

    return ret;
}

TError SanitizeLayer(TPath layer, bool merge) {
    std::vector<std::string> content;

    TError error = layer.ReadDirectory(content);
    if (error)
        return error;

    for (auto entry: content) {
        TPath path = layer.AddComponent(entry);

        /* Handle aufs whiteouts */
        if (entry.compare(0, 4, ".wh.") == 0) {
            error = path.Unlink();
            if (error)
                return error;

            path = layer.AddComponent(entry.substr(4));
            if (path.Exists()) {
                if (path.GetType() == EFileType::Directory) {
                    error = path.ClearDirectory();
                    if (!error)
                        error = path.Rmdir();
                } else
                    error = path.Unlink();
                if (error)
                    return error;
            }

            if (!merge) {
                /* Convert into overlayfs whiteout */
                error = path.Mknod(S_IFCHR, 0);
                if (error)
                    return error;
            }

            continue;
        }

        if (path.GetType() == EFileType::Directory) {
            error = SanitizeLayer(path, merge);
            if (error)
                return error;
        }
    }
    return TError::Success();
}
