#include <memory>

#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "util/unix.hpp"
#include "util/sha256.hpp"
#include "config.hpp"

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
        }
        return TError::Success();
    }

    bool Save(kv::TNode &node) override {
        auto a = node.add_pairs();
        a->set_key("loop_dev");
        a->set_val(std::to_string(LoopDev));
        return true;
    }

    void Restore(kv::TNode &node) override {
        for (int i = 0; i < node.pairs_size(); i++) {
            auto key = node.pairs(i).key();
            auto value = node.pairs(i).val();

            if (key == "loop_dev") {
                TError error = StringToInt(value, LoopDev);
                if (error)
                    L_WRN() << "Can't restore loop device number: " << value << std::endl;
            }
        }

        TError error = Create();
        if (error)
            L_WRN() << "Can't restore loop device number: " << LoopDev << std::endl;
    }

    TError Construct() const override {
        TError error;
        TFile loopFile(LoopPath);

        if (LoopDev >= 0) {
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
            TError error = m.Umount();
            TError firstError;
            if (error && !firstError)
                firstError = error;

            TFile img(LoopPath);
            error = img.Remove();
            if (error && !firstError)
                firstError = error;
        } else {
            TFolder f(Volume->GetPath());
            TError error = f.Remove(true);
            if (error)
                return error;
        }

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

    bool Save(kv::TNode &node) override {
        return false;
    }

    void Restore(kv::TNode &node) override {
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

TError TVolume::Create(std::shared_ptr<TKeyValueStorage> storage) {
    TError ret;
    TFolder dir(Path);

    SetValid(false);

    if (!Path.ToString().length() || Path.ToString()[0] != '/')
        return TError(EError::InvalidValue, "Invalid volume path");

    ret = StringWithUnitToUint64(Quota, ParsedQuota);
    if (ret)
        return TError(EError::InvalidValue, "Invalid volume quota");

    ret = Holder->IdMap.Get(Id);
    if (ret)
        return ret;

    KvNode = storage->GetNode(Id);

    ret = Holder->Insert(shared_from_this());
    if (ret)
        goto put_id;

    if (Path.Exists()) {
        ret = TError(EError::InvalidValue, "Destination path already exists");
        goto remove_volume;
    }

    ret = dir.Create(0755, false);
    if (ret)
        goto remove_volume;

    ret = Path.Chown(Cred);
    if (ret)
        goto remove_volume;

    ret = Prepare();
    if (ret)
        goto remove_volume;
    Impl->Create();

    ret = SaveToStorage();
    if (ret)
        goto destroy_volume;

    return TError::Success();

destroy_volume:
    Impl->Destroy();

remove_volume:
    Holder->Remove(shared_from_this());

put_id:
    (void)Holder->IdMap.Put(Id);
    return ret;
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
    Holder->Remove(shared_from_this());
    KvNode->Remove();
    Impl->Destroy();
    Impl = nullptr;
    return TError::Success();
}

TError TVolume::SaveToStorage() const {
    TError error = KvNode->Append("path", Path.ToString());
    if (error)
        return error;

    error = KvNode->Append("source", Resource->GetSource().ToString());
    if (error)
        return error;

    error = KvNode->Append("quota", Quota);
    if (error)
        return error;

    error = KvNode->Append("flags", Flags);
    if (error)
        return error;

    error = KvNode->Append("user", Cred.UserAsString());
    if (error)
        return error;

    error = KvNode->Append("group", Cred.GroupAsString());
    if (error)
        return error;

    error = KvNode->Append("id", std::to_string(Id));
    if (error)
        return error;

    kv::TNode node;
    if (Impl->Save(node))
        error = KvNode->Append(node);
    return error;
}

TError TVolume::LoadFromStorage() {
    kv::TNode node;
    TError error;
    std::string user, group, source, quota, flags;

    error = KvNode->Load(node);
    if (error)
        return error;

    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        if (key == "path")
            Path = value;
        else if (key == "source")
            source = value;
        else if (key == "quota")
            quota = value;
        else if (key == "flags")
            flags = value;
        else if (key == "user")
            user = value;
        else if (key == "group")
            group = value;
        else if (key == "id") {
            uint32_t id32;
            TError error = StringToUint32(value, id32);
            if (error)
                return error;

            Id = (uint16_t)id32;
        }
    }

    error = Holder->IdMap.GetAt(Id);
    if (error)
        return error;

    error = Holder->GetResource(source, Resource);
    if (error)
        return error;

    error = Prepare();
    if (error)
        return error;
    Impl->Restore(node);

    if (quota.empty())
        return TError(EError::InvalidValue, "Volume " + Path.ToString() + " info isn't full");

    error = Cred.Parse(user, group);
    if (error)
        return TError(EError::InvalidValue, "Bad volume " + Path.ToString() + " credentials: " +
                      user + " " + group);

    Quota = quota;
    Flags = flags;

    error = Holder->Insert(shared_from_this());
    if (error)
        return error;

    SetValid(true);

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
        std::shared_ptr<TVolume> v = std::make_shared<TVolume>(i, shared_from_this());

        error = v->LoadFromStorage();
        if (error) {
            i->Remove();
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
