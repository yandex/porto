#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "config.hpp"

/* TVolumeHolder */

TError TVolumeHolder::Insert(std::shared_ptr<TVolume> volume) {
    if (Volumes.find(volume->GetName()) == Volumes.end()) {
        Volumes[volume->GetName()] = volume;
        return TError::Success();
    }

    return TError(EError::VolumeAlreadyExists, "volume " + volume->GetName() +
                  "already exists");
}

void TVolumeHolder::Remove(std::shared_ptr<TVolume> volume) {
    Volumes.erase(volume->GetName());
}

std::shared_ptr<TVolume> TVolumeHolder::Get(const std::string &name) {
    auto v = Volumes.find(name);
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

    if (CheckQuota())
        return TError(EError::InvalidValue, "Volume " + Name + " has invalid quota.");

    ret = Holder->Insert(shared_from_this());
    if (ret)
        return ret;

    ret = SaveToStorage();
    if (ret) {
        Holder->Remove(shared_from_this());
        return ret;
    }

    if (!Source.empty()) {
        TPath f(Source);
        if (!f.Exists()) {
            Holder->Remove(shared_from_this());
            return TError(EError::InvalidValue, "Volume " + Name + " has non-existing source.");
        }
        if (f.GetType() != EFileType::Regular) {
            Holder->Remove(shared_from_this());
            return TError(EError::InvalidValue, "Volume's " + Name + " source isn't a regular file.");
        }
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

TError TVolume::CheckQuota() {
    if (Quota.empty()) {
        Quota = "0";
        ParsedQuota = 0;
        return TError::Success();
    }

    return StringWithUnitToUint64(Quota, ParsedQuota);
}

TError TVolume::Destroy() {
    Holder->Remove(shared_from_this());
    Storage->RemoveNode(Name);
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

    return Storage->SaveNode(Name, node);
}

TError TVolume::LoadFromStorage() {
    kv::TNode node;
    TError error;
    std::string user, group, source, quota, flags;

    error = Storage->LoadNode(Name, node);
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

    if (quota.empty())
        return TError(EError::InvalidValue, "Volume " + Name + " info isn't full");

    error = Cred.Parse(user, group);
    if (error)
        return TError(EError::InvalidValue, "Bad volume " + Name + " credentials: " +
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

        L() << "Volume " << v->GetName() << " restored." << std::endl;
    }

    return TError::Success();
}
