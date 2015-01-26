#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "util/unix.hpp"
#include "config.hpp"

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

    if (ParsedQuota > 0) {
        ret = GetLoopDev(LoopDev);
        if (ret)
            goto remove_volume;
    }

    ret = SaveToStorage();
    if (ret)
        goto remove_volume;

    return TError::Success();

remove_volume:
    Holder->Remove(shared_from_this());
    return ret;
}

TPath TVolume::GetLoopPath() const {
    TPath path = config().volumes().tmp_dir();
    path.AddComponent(std::to_string(LoopDev) + ".img");
    return path;
}

TError TVolume::Construct() const {
    int status;
    TError error;
    TLoopMount m(GetLoopPath(), Path, "ext4", LoopDev);
    TFile loopFile(GetLoopPath());

    if (LoopDev >= 0) {
        error = AllocLoop(GetLoopPath(), ParsedQuota);
        if (error)
            return error;

        error = m.Mount();
        if (error)
            goto remove_loop;
    }

    error = Run({ "tar", "xf", Source, "-C", Path }, status);
    if (error)
        goto umount_loop;
    if (status) {
        error = TError(EError::Unknown, "Can't execute tar " + std::to_string(status));
        goto umount_loop;
    }

    return TError::Success();

umount_loop:
    if (ParsedQuota) {
        TError ret = m.Umount();
        if (ret)
            L_ERR() << "Can't construct volume: " << ret << std::endl;

        ret = loopFile.Remove();
        if (ret)
            L_ERR() << "Can't construct volume: " << ret << std::endl;
    }

remove_loop:
    TFolder dir(Path);
    TError ret = dir.Remove();
    if (ret)
        L_ERR() << "Can't construct volume: " << ret << std::endl;
    return error;
}

TError TVolume::Deconstruct() const {
    if (LoopDev >= 0) {
        TLoopMount m(GetLoopPath(), Path, "ext4", LoopDev);
        TError error = m.Umount();
        if (error)
            L_ERR() << "Can't umount volume " << Path << ": " << error << std::endl;

        TFile img(GetLoopPath());
        error = img.Remove();
        if (error)
            L_ERR() << "Can't remove volume loop image at " << GetLoopPath().ToString() << ": " << error << std::endl;
    } else {
        TFolder f(Path);
        TError error = f.Remove(true);
        if (error)
            L_ERR() << "Can't deconstruct volume " << Path << ": " << error << std::endl;
    }

    return TError::Success();
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
    if (LoopDev >= 0) {
        TError error = PutLoopDev(LoopDev);
        if (error)
            return error;
    }
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

    a = node.add_pairs();
    a->set_key("loop_dev");
    a->set_val(std::to_string(LoopDev));

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
        } else if (key == "loop_dev") {
            TError error = StringToInt(value, LoopDev);
            if (error)
                L_WRN() << "Can't restore loop device number: " << value << std::endl;
        } else
            L_WRN() << "Unknown key in volume storage: " << key << std::endl;
    }

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
