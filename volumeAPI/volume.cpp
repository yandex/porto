#include <memory>

#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "util/unix.hpp"
#include "config.hpp"

TError TVolumeImpl::Untar(const TPath &what, const TPath &where) const {
    int status;

    TError error = Run({ "tar", "xf", what.ToString(), "-C", where.ToString() }, status);
    if (error)
        return error;
    if (status)
        return TError(EError::Unknown, "Can't execute tar " + std::to_string(status));

    return TError::Success();
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

            LoopPath = config().volumes().tmp_dir();
            LoopPath.AddComponent(std::to_string(LoopDev) + ".img");
        }
        return TError::Success();
    }

    TError Destroy() override {
        if (LoopDev >= 0) {
            TError error = PutLoopDev(LoopDev);
            if (error)
                return error;
        }
        return TError::Success();
    }

    void Save(kv::TNode &node) override {
        auto a = node.add_pairs();
        a = node.add_pairs();
        a->set_key("loop_dev");
        a->set_val(std::to_string(LoopDev));
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
                TError ret = dir.Remove();
                if (ret)
                    L_ERR() << "Can't construct volume: " << ret << std::endl;
                return error;
            }
        }

        error = Untar(Volume->GetSource(), Volume->GetPath());
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
            if (error)
                L_ERR() << "Can't umount volume " << Volume->GetPath() << ": " << error << std::endl;

            TFile img(LoopPath);
            error = img.Remove();
            if (error)
                L_ERR() << "Can't remove volume loop image at " << LoopPath.ToString() << ": " << error << std::endl;
        } else {
            TFolder f(Volume->GetPath());
            TError error = f.Remove(true);
            if (error)
                L_ERR() << "Can't deconstruct volume " << Volume->GetPath() << ": " << error << std::endl;
        }

        return TError::Success();
    }
};

class TVolumeNativeImpl : public TVolumeImpl {
    TPath OvlUpper;
    TPath OvlLower;
    TMount OvlMount;

public:
    TVolumeNativeImpl(std::shared_ptr<TVolume> volume) : TVolumeImpl(volume) {}

    TError Create() override {
        OvlUpper = config().volumes().tmp_dir();
        OvlUpper.AddComponent("upper");
        OvlLower = config().volumes().tmp_dir();
        OvlLower.AddComponent("lower");
        OvlMount = TMount("overlayfs", Volume->GetPath(), "overlayfs", {"lowerdir=" + OvlLower.ToString(), "upperdir=" + OvlUpper.ToString() });
        return TError::Success();
    }

    TError Destroy() override {
        return TError::Success();
    }

    void Save(kv::TNode &node) override {
    }

    void Restore(kv::TNode &node) override {
    }

    TError Construct() const override {
        TFolder workDir(Volume->GetPath());
        TFolder upperDir(OvlUpper);
        TFolder lowerDir(OvlLower);

        TError error = upperDir.Create(0755, true);
        if (error) {
            (void)Deconstruct();
            return error;
        }

        error = lowerDir.Create(0755, true);
        if (error) {
            (void)Deconstruct();
            return error;
        }

        error = Untar(Volume->GetSource(), OvlLower);
        if (error) {
            (void)Deconstruct();
            return error;
        }

        error = workDir.Create(0755, true);
        if (error) {
            (void)Deconstruct();
            return error;
        }

        error = OvlMount.Mount();
        if (error) {
            (void)Deconstruct();
            return error;
        }

        // TODO set project quota:
        /*
           struct if_dqblk quota;
           unsigned project_id = FIXME;
           quota.dqb_bhardlimit = ParsedQuota;
           ret = init_project_quota(OvlUpper.ToString().c_str());
           ret = set_project_id(OvlUpper.ToString().c_str(), project_id);
           set_project_quota(OvlUpper.ToString().c_str(), &quota);
           project_quota_on(OvlUpper.ToString());
           */
        return TError::Success();
    }

    TError Deconstruct() const override {
        TFolder workDir(Volume->GetPath());
        TFolder upperDir(OvlUpper);
        TFolder lowerDir(OvlLower);

        TError error = OvlMount.Umount();
        if (error)
            L_ERR() << "Can't deconstruct volume: " << error << std::endl;

        error = workDir.Remove(true);
        if (error)
            L_ERR() << "Can't deconstruct volume: " << error << std::endl;

        error = lowerDir.Remove(true);
        if (error)
            L_ERR() << "Can't deconstruct volume: " << error << std::endl;

        error = upperDir.Remove(true);
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

    return TError(EError::VolumeAlreadyExists, "volume " + volume->GetPath() +
                  "already exists");
}

void TVolumeHolder::Remove(std::shared_ptr<TVolume> volume) {
    Volumes.erase(volume->GetPath());
}

std::shared_ptr<TVolume> TVolumeHolder::Get(const std::string &path) {
    auto v = Volumes.find(path);
    if (v != Volumes.end())
        return v->second;
    else
        return nullptr;
}

std::vector<std::string> TVolumeHolder::List() const {
    std::vector<std::string> ret;

    for (auto v : Volumes)
        ret.push_back(v.first);

    return ret;
}

/* TVolume */

void TVolume::CreateImpl() {
    if (config().volumes().native())
        Impl = std::unique_ptr<TVolumeImpl>(new TVolumeNativeImpl(shared_from_this()));
    else
        Impl = std::unique_ptr<TVolumeImpl>(new TVolumeLoopImpl(shared_from_this()));
}

TError TVolume::Create() {
    TError ret;
    TPath srcPath(Source), dstPath(Path);
    TFolder dir(dstPath);

    if (!Path.length() || Path[0] != '/')
        return TError(EError::InvalidValue, "Invalid volume path");

    if (!Source.length() || Source[0] != '/')
        return TError(EError::InvalidValue, "Invalid volume source");

    ret = StringWithUnitToUint64(Quota, ParsedQuota);
    if (ret)
        return TError(EError::InvalidValue, "Invalid volume quota");

    ret = Holder->Insert(shared_from_this());
    if (ret)
        return ret;

    if (!srcPath.Exists()) {
        ret = TError(EError::InvalidValue, "Source doesn't exist");
        goto remove_volume;
    }

    if (srcPath.GetType() != EFileType::Regular) {
        ret = TError(EError::InvalidValue, "Source isn't a regular file");
        goto remove_volume;
    }

    if (dstPath.Exists()) {
        ret = TError(EError::InvalidValue, "Destination path already exists");
        goto remove_volume;
    }

    ret = dir.Create();
    if (ret)
        goto remove_volume;

    ret = dstPath.Chown(Cred.UserAsString(), Cred.GroupAsString());
    if (ret)
        goto remove_volume;

    CreateImpl();
    Impl->Create();

    ret = SaveToStorage();
    if (ret)
        goto remove_volume;

    return TError::Success();

remove_volume:
    Holder->Remove(shared_from_this());
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

TError TVolume::Destroy() {
    Holder->Remove(shared_from_this());
    Storage->RemoveNode(Path);
    Impl->Destroy();
    Impl = nullptr;
    return TError::Success();
}

TError TVolume::SaveToStorage() const {
    kv::TNode node;

    auto a = node.add_pairs();
    a->set_key("source");
    a->set_val(Source);

    a = node.add_pairs();
    a->set_key("quota");
    a->set_val(Quota);

    a = node.add_pairs();
    a->set_key("flags");
    a->set_val(Flags);

    a = node.add_pairs();
    a->set_key("user");
    a->set_val(Cred.UserAsString());

    a = node.add_pairs();
    a->set_key("group");
    a->set_val(Cred.GroupAsString());

    Impl->Save(node);

    return Storage->SaveNode(Path, node);
}

TError TVolume::LoadFromStorage() {
    kv::TNode node;
    TError error;
    std::string user, group, source, quota, flags;

    error = Storage->LoadNode(Path, node);
    if (error)
        return error;

    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        if (key == "source") {
            source = value;
        } else if (key == "quota") {
            quota = value;
        } else if (key == "flags") {
            flags = value;
        } else if (key == "user") {
            user = value;
        } else if (key == "group") {
            group = value;
        } else
            L_WRN() << "Unknown key in volume storage: " << key << std::endl;
    }

    CreateImpl();
    Impl->Restore(node);

    if (quota.empty())
        return TError(EError::InvalidValue, "Volume " + Path + " info isn't full");

    error = Cred.Parse(user, group);
    if (error)
        return TError(EError::InvalidValue, "Bad volume " + Path + " credentials: " +
                      user + " " + group);

    Source = source;
    Quota = quota;
    Flags = flags;

    error = Holder->Insert(shared_from_this());
    if (error)
        return error;

    return TError::Success();
}

TError TVolumeHolder::RestoreFromStorage() {
    std::vector<std::string> list;

    TPath volumes = config().volumes().tmp_dir();
    if (!volumes.Exists() || volumes.GetType() != EFileType::Directory) {
        TFolder dir(config().volumes().tmp_dir());
        (void)dir.Remove(true);
        TError error = dir.Create(0755, true);
        if (error)
            return error;
    }

    TError error = Storage->ListNodes(list);
    if (error)
        return error;

    for (auto &i : list) {
        std::shared_ptr<TVolume> v = std::make_shared<TVolume>(Storage, shared_from_this(), i);

        error = v->LoadFromStorage();
        if (error) {
            Storage->RemoveNode(i);
            L_WRN() << "Corrupted volume " << i << " removed. " << error << std::endl;
        }

        L() << "Volume " << v->GetPath() << " restored." << std::endl;
    }

    return TError::Success();
}
