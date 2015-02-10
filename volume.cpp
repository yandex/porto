#include <memory>

#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "util/unix.hpp"
#include "util/sha256.hpp"
#include "config.hpp"

extern "C" {
#include <sys/vfs.h>
}

void RegisterVolumeProperties(std::shared_ptr<TRawValueMap> m) {
    m->Add(V_PATH, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_SOURCE, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_QUOTA, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_FLAGS, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_USER, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_GROUP, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_ID, new TIntValue(PERSISTENT_VALUE));
    m->Add(V_VALID, new TBoolValue(PERSISTENT_VALUE));
    m->Add(V_LOOP_DEV, new TIntValue(PERSISTENT_VALUE));
}

class TVolumeLoopImpl : public TVolumeImpl {
    int LoopDev = -1;
    TPath LoopPath;

public:
    TVolumeLoopImpl(std::shared_ptr<TVolume> volume) : TVolumeImpl(volume) {}

    TError Create() override {
        if (Volume->GetParsedQuota() > 0) {
            if (LoopDev < 0) {
                TError error = GetLoopDev(LoopDev);
                if (error)
                    return error;
            }

            LoopPath = TPath(config().volumes().volume_dir()).AddComponent(std::to_string(LoopDev) + ".img");
        }
        return TError::Success();
    }

    TError Destroy() override {
        if (LoopDev >= 0) {
            (void)PutLoopDev(LoopDev);

            TFile img(LoopPath);
            (void)img.Remove();
        }
        return TError::Success();
    }

    TError Save(std::shared_ptr<TValueMap> data) override {
        return data->Set<int>(V_LOOP_DEV, LoopDev);
    }

    TError Restore(std::shared_ptr<TValueMap> data) override {
        LoopDev = data->Get<int>(V_LOOP_DEV);
        return Create();
    }

    TError Construct() const override {
        TError error;
        TFile loopFile(LoopPath);

        if (LoopDev >= 0) {
            L() << "Allocate loop image with size " << Volume->GetParsedQuota() << std::endl;
            error = AllocLoop(LoopPath, Volume->GetParsedQuota());
            if (error)
                return error;

            TLoopMount m = TLoopMount(LoopPath, Volume->GetPath(), "ext4", LoopDev);
            error = m.Mount();
            if (error) {
                TFolder dir(Volume->GetPath());
                (void)dir.Remove();
                return error;
            }
        }

        error = Volume->GetResource()->Copy(Volume->GetPath());
        if (error) {
            (void)Deconstruct();
            return error;
        }

        return TError::Success();
    }

    TError Deconstruct() const override {
        if (LoopDev >= 0) {
            TLoopMount m = TLoopMount(LoopPath, Volume->GetPath(), "ext4", LoopDev);
            return m.Umount();
        } else {
            TFolder f(Volume->GetPath());
            TError error = f.Remove(true);
            if (error)
                return error;
        }

        return TError::Success();
    }

    TError GetUsage(uint64_t &used, uint64_t &avail) const {
        struct statfs st;
        int ret = statfs(Volume->GetPath().ToString().c_str(), &st);
        if (ret)
            return TError(EError::Unknown, errno, "statvfs(" + Volume->GetPath().ToString() + ")");

        used = (st.f_blocks - st.f_bfree) * st.f_frsize;
        avail = st.f_bfree * st.f_frsize;

        return TError::Success();
    }
};

class TVolumeNativeImpl : public TVolumeImpl {
    TPath OvlUpper;
    TPath OvlWork;
    TPath OvlLower;
    TMount OvlMount;

public:
    TVolumeNativeImpl(std::shared_ptr<TVolume> volume) : TVolumeImpl(volume) {}

    TError Create() override {
        std::string id = std::to_string(Volume->GetId());

        OvlUpper = TPath(config().volumes().volume_dir()).AddComponent(id).AddComponent("upper");
        OvlWork = TPath(config().volumes().volume_dir()).AddComponent(id).AddComponent("work");
        OvlLower = Volume->GetResource()->GetPath();
        OvlMount = TMount("overlay", Volume->GetPath(), "overlay", {"lowerdir=" + OvlLower.ToString(), "upperdir=" + OvlUpper.ToString(), "workdir=" + OvlWork.ToString() });
        return TError::Success();
    }

    TError Destroy() override {
        return TError::Success();
    }

    TError Save(std::shared_ptr<TValueMap> data) override {
        return TError::Success();
    }
    TError Restore(std::shared_ptr<TValueMap> data) override {
        return TError::Success();
    }

    TError SetQuota(const TPath &path, uint64_t quota) const {
        /*
           struct if_dqblk quota;
           unsigned project_id = FIXME;
           quota.dqb_bhardlimit = quota;
           ret = init_project_quota(path.ToString().c_str());
           ret = set_project_id(path.ToString().c_str(), project_id);
           set_project_quota(path.ToString().c_str(), &quota);
           project_quota_on(path.ToString());
           */
        return TError::Success();
    }

    TError Construct() const override {
        TFolder upperDir(OvlUpper);
        TFolder workDir(OvlWork);

        TError error = upperDir.Create(0755, true);
        if (error) {
            (void)Deconstruct();
            return error;
        }

        error = OvlUpper.Chown(Volume->GetCred());
        if (error)
            return error;

        error = workDir.Create(0755, true);
        if (error) {
            (void)Deconstruct();
            return error;
        }

        error = OvlWork.Chown(Volume->GetCred());
        if (error)
            return error;

        error = Volume->GetResource()->Create();
        if (error) {
            (void)Deconstruct();
            return error;
        }

        error = OvlMount.Mount();
        if (error) {
            (void)Deconstruct();
            return error;
        }

        return SetQuota(OvlUpper, Volume->GetParsedQuota());
    }

    TError Deconstruct() const override {
        TFolder upperDir(OvlUpper);
        TFolder workDir(OvlWork);

        TError error = OvlMount.Umount();
        if (error)
            L_ERR() << "Can't deconstruct volume: " << error << std::endl;

        error = workDir.Remove(true);
        if (error)
            L_ERR() << "Can't deconstruct volume: " << error << std::endl;

        error = upperDir.Remove(true);
        if (error)
            L_ERR() << "Can't deconstruct volume: " << error << std::endl;

        TFolder dir(Volume->GetPath());
        error = dir.Remove(true);
        if (error)
            L_ERR() << "Can't deconstruct volume: " << error << std::endl;

        return TError::Success();
    }

    TError GetUsage(uint64_t &used, uint64_t &avail) const {
        struct statfs st;
        int ret = statfs(Volume->GetPath().ToString().c_str(), &st);
        if (ret)
            return TError(EError::Unknown, errno, "statvfs(" + Volume->GetPath().ToString() + ")");

        used = (st.f_blocks - st.f_bfree) * st.f_frsize;
        // TODO: get info from kernel quota subsystem
        avail = st.f_bfree * st.f_frsize;

        return TError::Success();
    }
};

/* TVolumeHolder */

TError TVolumeHolder::Insert(std::shared_ptr<TVolume> volume) {
    if (Volumes.find(volume->GetPath()) == Volumes.end()) {
        Volumes[volume->GetPath()] = volume;
        return TError::Success();
    }

    return TError(EError::VolumeAlreadyExists, "Volume already exists");
}

void TVolumeHolder::Remove(std::shared_ptr<TVolume> volume) {
    Volumes.erase(volume->GetPath());
}

std::shared_ptr<TVolume> TVolumeHolder::Get(const TPath &path) {
    auto v = Volumes.find(path);
    if (v != Volumes.end())
        return v->second;
    else
        return nullptr;
}

std::vector<TPath> TVolumeHolder::List() const {
    std::vector<TPath> ret;

    for (auto v : Volumes)
        ret.push_back(v.first);

    return ret;
}

/* TVolume */

TError TVolume::Prepare() {
    if (config().volumes().native())
        Impl = std::unique_ptr<TVolumeImpl>(new TVolumeNativeImpl(shared_from_this()));
    else
        Impl = std::unique_ptr<TVolumeImpl>(new TVolumeLoopImpl(shared_from_this()));

    PORTO_ASSERT(Resource);

    TError error = Resource->Prepare();
    if (error)
        return error;

    return TError::Success();
}

bool TVolume::IsValid() const {
    return Data->Get<bool>(V_VALID);
}

TError TVolume::SetValid(bool v) {
    return Data->Set<bool>(V_VALID, v);
}

TError TVolume::ParseQuota(const std::string &quota) {
    TError error = StringWithUnitToUint64(quota, ParsedQuota);
    if (error)
        return TError(EError::InvalidValue, "Invalid volume quota");
    return TError::Success();
}

TError TVolume::Create(std::shared_ptr<TKeyValueStorage> storage,
                       const TPath &path,
                       std::shared_ptr<TResource> resource,
                       const std::string &quota,
                       const std::string &flags) {
    uint16_t id;
    TError error;

    if (!path.ToString().length() || path.ToString()[0] != '/')
        return TError(EError::InvalidValue, "Invalid volume path");

    error = ParseQuota(quota);
    if (error)
        return error;

    error = Holder->IdMap.Get(id);
    if (error)
        return error;

    KvNode = storage->GetNode(id);
    Resource = resource;
    Data = std::make_shared<TValueMap>(KvNode);
    RegisterVolumeProperties(Data);

    error = Data->Set<std::string>(V_PATH, path.ToString());
    if (error)
        return error;
    error = Data->Set<std::string>(V_SOURCE, Resource->GetSource().ToString());
    if (error)
        return error;
    error = Data->Set<std::string>(V_QUOTA, quota);
    if (error)
        return error;
    error = Data->Set<std::string>(V_FLAGS, flags);
    if (error)
        return error;
    error = Data->Set<std::string>(V_USER, Cred.UserAsString());
    if (error)
        return error;
    error = Data->Set<std::string>(V_GROUP, Cred.GroupAsString());
    if (error)
        return error;
    error = Data->Set<int>(V_ID, id);
    if (error)
        return error;

    TFolder dir(path);

    error = Holder->Insert(shared_from_this());
    if (error)
        goto put_id;

    if (dir.GetPath().Exists()) {
        error = TError(EError::InvalidValue, "Destination path already exists");
        goto remove_volume;
    }

    error = dir.Create(0755, false);
    if (error)
        goto remove_volume;

    error = dir.GetPath().Chown(Cred);
    if (error)
        goto remove_volume;

    error = Prepare();
    if (error)
        goto remove_volume;

    error = Impl->Create();
    if (error)
        goto remove_volume;

    error = Impl->Save(Data);
    if (error)
        goto destroy_volume;

    return TError::Success();

destroy_volume:
    (void)Impl->Destroy();

remove_volume:
    Holder->Remove(shared_from_this());

put_id:
    (void)Holder->IdMap.Put(id);
    return error;
}

TError TVolume::Construct() const {
    return Impl->Construct();
}

TError TVolume::Deconstruct() const {
    return Impl->Deconstruct();
}

TError TVolume::CheckPermission(const TCred &ucred) const {
    if (ucred.IsPrivileged())
        return TError::Success();

    if (Cred == ucred)
        return TError::Success();

    return TError(EError::Permission, "Permission error");
}

const std::string TVolume::GetSource() const {
    return Resource->GetSource().ToString();
}

TError TVolume::Destroy() {
    if (Holder)
        Holder->Remove(shared_from_this());
    if (KvNode)
        KvNode->Remove();
    if (Impl) {
        Impl->Destroy();
        Impl = nullptr;
    }
    return TError::Success();
}

TError TVolume::GetUsage(uint64_t &used, uint64_t &avail) const {
    if (!Impl)
        return TError::Success();

    return Impl->GetUsage(used, avail);
}

TError TVolume::LoadFromStorage() {
    Data = std::make_shared<TValueMap>(KvNode);
    RegisterVolumeProperties(Data);

    TError error = Data->Restore();
    if (error)
        return error;

    if (!Data->Get<bool>(V_VALID))
        return TError(EError::Unknown, "Invalid volume");

    error = Holder->IdMap.GetAt((uint16_t)Data->Get<int>(V_ID));
    if (error)
        return error;

    error = Holder->GetResource(Data->Get<std::string>(V_SOURCE), Resource);
    if (error)
        return error;

    error = Prepare();
    if (error)
        return error;

    error = Impl->Restore(Data);
    if (error)
        return error;

    if (GetQuota().empty())
        return TError(EError::InvalidValue, "Volume " + GetPath().ToString() + " info isn't full");

    error = Cred.Parse(Data->Get<std::string>(V_USER), Data->Get<std::string>(V_GROUP));
    if (error)
        return TError(EError::InvalidValue, "Bad volume " + GetPath().ToString() + " credentials: " +
                      Data->Get<std::string>(V_USER) + " " +
                      Data->Get<std::string>(V_GROUP));

    error = Holder->Insert(shared_from_this());
    if (error)
        return error;

    error = ParseQuota(GetQuota());
    if (error)
        return error;

    error = SetValid(true);
    if (error)
        return error;

    return TError::Success();
}

TError TVolumeHolder::RestoreFromStorage() {
    std::vector<std::shared_ptr<TKeyValueNode>> list;

    TPath volumes = config().volumes().volume_dir();
    if (!volumes.Exists() || volumes.GetType() != EFileType::Directory) {
        TFolder dir(config().volumes().volume_dir());
        (void)dir.Remove(true);
        TError error = dir.Create(0755, true);
        if (error)
            return error;
    }

    TError error = Storage->ListNodes(list);
    if (error)
        return error;

    for (auto &i : list) {
        L() << "Restore volume " << i->GetName() << std::endl;

        std::shared_ptr<TVolume> v = std::make_shared<TVolume>(i, shared_from_this());

        error = v->LoadFromStorage();
        if (error) {
            (void)i->Remove();
            (void)v->Destroy();
            L_WRN() << "Corrupted volume " << i << " removed: " << error << std::endl;
            continue;
        }

        L() << "Volume " << v->GetPath() << " restored" << std::endl;
    }

    RemoveIf(config().volumes().resource_dir(),
             EFileType::Directory,
             [&](const std::string &name, const TPath &path) {
                return Resources.find(path) == Resources.end();
             });

    RemoveIf(config().volumes().volume_dir(),
             EFileType::Directory,
             [&](const std::string &name, const TPath &path) {
                bool found = false;
                for (auto v : Volumes)
                    if (std::to_string(v.second->GetId()) == name)
                        found = true;
                return !found;
             });

    return TError::Success();
}

void TVolumeHolder::Destroy() {
    while (Volumes.begin() != Volumes.end()) {
        auto name = Volumes.begin()->first;
        auto volume = Volumes.begin()->second;
        TError error = volume->Deconstruct();
        if (error)
            L_ERR() << "Can't deconstruct volume " << name << ": " << error << std::endl;

        error = volume->Destroy();
        if (error)
            L_ERR() << "Can't destroy volume " << name << ": " << error << std::endl;
    }
}

TError TVolumeHolder::GetResource(const TPath &path, std::shared_ptr<TResource> &resource) {
    if (!path.ToString().length() || path.ToString()[0] != '/')
        return TError(EError::InvalidValue, "Invalid source");

    TPath srcPath(path);
    if (!srcPath.Exists())
        return TError(EError::InvalidValue, "Source doesn't exist");

    if (srcPath.GetType() != EFileType::Regular)
        return TError(EError::InvalidValue, "Source isn't a regular file");

    resource = nullptr;
    auto weak = Resources.find(path.ToString());
    if (weak != Resources.end()) {
        resource = weak->second.lock();
        if (!resource)
            Resources.erase(path.ToString());
    }

    if (!resource) {
        resource = std::make_shared<TResource>(path);
        Resources[path.ToString()] = resource;
    }

    return TError::Success();
}

TError TResource::Untar(const TPath &what, const TPath &where) const {
    int status;

    TError error = Run({ "tar", "xf", what.ToString(), "-C", where.ToString() }, status);
    if (error)
        return error;
    if (status)
        return TError(EError::Unknown, "Can't execute tar " + std::to_string(status));

    return TError::Success();
}

TError TResource::Prepare() {
    Path = TPath(config().volumes().resource_dir()).AddComponent(Sha256(Path.ToString()));

    TFolder dir(Path);
    if (!dir.Exists())
        return dir.Create(0755, true);

    return TError::Success();
}

TError TResource::Create() const {
    TPath p = Path.AddComponent(".done");

    if (!p.Exists()) {
        TError error = Untar(Source, Path);
        if (error)
            return error;

        TFile f(p);
        return f.Touch();
    }

    return TError::Success();
}

TError TResource::Copy(const TPath &to) const {
    TError error = Create();
    if (error)
        return error;

    TFolder dir(Path);
    return dir.Copy(to);
}

TError TResource::Destroy() const {
    L() << "Destroy resource " << Path << std::endl;

    if (Path.Exists()) {
        TFolder dir(Path);
        return dir.Remove(true);
    }
    return TError::Success();
}

TResource::~TResource() {
    TError error = Destroy();
    if (error)
        L_ERR() << "Can't destroy resource " << Source << " at " << Path << ": " << error << std::endl;
}
