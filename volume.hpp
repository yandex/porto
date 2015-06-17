#pragma once

#include <string>
#include <set>

#include "kvalue.hpp"
#include "common.hpp"
#include "value.hpp"
#include "util/mount.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"

constexpr const char *V_PATH = "path";
constexpr const char *V_BACKEND = "backend";
constexpr const char *V_READY = "ready";

constexpr const char *V_ID = "_id";
constexpr const char *V_CONTAINERS = "_containers";
constexpr const char *V_LOOP_DEV = "_loop_dev";
constexpr const char *V_AUTO_PATH = "_auto_path";

constexpr const char *V_USER = "user";
constexpr const char *V_GROUP = "group";
constexpr const char *V_PERMISSIONS = "permissions";
constexpr const char *V_CREATOR = "creator";

constexpr const char *V_STORAGE = "storage";
constexpr const char *V_LAYERS = "layers";
constexpr const char *V_READ_ONLY = "read_only";

constexpr const char *V_SPACE_LIMIT = "space_limit";
constexpr const char *V_INODE_LIMIT = "inode_limit";
constexpr const char *V_SPACE_GUARANTEE = "space_guarantee";
constexpr const char *V_INODE_GUARANTEE = "inode_guarantee";

constexpr const char *V_SPACE_USED = "space_used";
constexpr const char *V_INODE_USED = "inode_used";
constexpr const char *V_SPACE_AVAILABLE = "space_available";
constexpr const char *V_INODE_AVAILABLE = "inode_available";

class TVolumeHolder;
class TVolume;
class TContainer;
class TContainerHolder;

TError SanitizeLayer(TPath layer, bool merge);

class TVolumeBackend {
protected:
    std::shared_ptr<TVolume> Volume;
public:
    TVolumeBackend(std::shared_ptr<TVolume> volume) : Volume(volume) {}
    virtual TError Configure(std::shared_ptr<TValueMap> Config);
    virtual TError Save(std::shared_ptr<TValueMap> Config);
    virtual TError Restore(std::shared_ptr<TValueMap> Config);
    virtual TError Build() =0;
    virtual TError Destroy() =0;
    virtual TError Clear();
    virtual TError Move(TPath dest);
    virtual TError Resize(uint64_t space_limit, uint64_t inode_limit);
    virtual TError GetStat(uint64_t &space_used, uint64_t &space_avail,
                           uint64_t &inode_used, uint64_t &inode_avail);
};

class TVolume : public std::enable_shared_from_this<TVolume>,
                public TLockable,
                public TNonCopyable {
    friend class TVolumeHolder;
    std::shared_ptr<TVolumeHolder> Holder;
    std::shared_ptr<TValueMap> Config;
    TCred Cred;
    unsigned Permissions;

    std::unique_ptr<TVolumeBackend> Backend;
    TError OpenBackend();

public:
    TVolume(std::shared_ptr<TVolumeHolder> holder,
            std::shared_ptr<TValueMap> config) :
        Holder(holder), Config(config) {}
    TError Configure(const TPath &path, const TCred &creator_cred,
                     std::shared_ptr<TContainer> creator_container,
                     const std::map<std::string, std::string> &properties);

    /* Protected with TVolume->Lock() */
    TError Build();
    TError Destroy();

    TError Restore();
    TError Clear();

    const std::vector<std::string> GetContainers() const {
        return Config->Get<std::vector<std::string>>(V_CONTAINERS);
    }
    TError LinkContainer(const std::string name);
    bool UnlinkContainer(const std::string name);

    TError CheckPermission(const TCred &ucred) const;

    std::string GetBackend() const { return Config->Get<std::string>(V_BACKEND); }
    TPath GetPath() const;
    bool IsAutoPath() const;
    TPath GetStorage() const;
    TPath GetInternal(std::string type) const;
    TPath GetChrootInternal(TPath container_root, std::string type) const;
    int GetId() const { return Config->Get<int>(V_ID); }
    bool IsReadOnly() const { return Config->Get<bool>(V_READ_ONLY); }

    /* Protected with TVolume->Lock() _and_ TVolumeHolder->Lock() */
    TError SetReady(bool ready) { return Config->Set<bool>(V_READY, ready); }
    bool IsReady() const { return Config->Get<bool>(V_READY); }

    TError Resize(uint64_t space_limit, uint64_t inode_limit);

    void GetGuarantee(uint64_t &space_guarantee, uint64_t &inode_guarantee) const {
        space_guarantee = Config->Get<uint64_t>(V_SPACE_GUARANTEE);
        inode_guarantee = Config->Get<uint64_t>(V_INODE_GUARANTEE);
    }

    void GetQuota(uint64_t &space_limit, uint64_t &inode_limit) const {
        space_limit = Config->Get<uint64_t>(V_SPACE_LIMIT);
        inode_limit = Config->Get<uint64_t>(V_INODE_LIMIT);
    }

    TError GetStat(uint64_t &space_used, uint64_t &space_avail,
                   uint64_t &inode_used, uint64_t &inode_avail) const;

    TError GetStat(uint64_t &space_used, uint64_t &space_avail) const {
        uint64_t inode_used, inode_avail;
        return GetStat(space_used, space_avail, inode_used, inode_avail);
    }

    std::vector<TPath> GetLayers() const;

    TCred GetCred() const { return Cred; }
    unsigned GetPermissions() const { return Permissions; }

    std::map<std::string, std::string> GetProperties();
};

class TVolumeHolder : public std::enable_shared_from_this<TVolumeHolder>,
                      public TLockable,
                      public TNonCopyable {
    std::shared_ptr<TKeyValueStorage> Storage;
    std::map<TPath, std::shared_ptr<TVolume>> Volumes;
    TIdMap IdMap;
public:
    TVolumeHolder(std::shared_ptr<TKeyValueStorage> storage) : Storage(storage) {}
    const std::vector<std::pair<std::string, std::string>> ListProperties();
    TError Create(std::shared_ptr<TVolume> &volume);
    void Remove(std::shared_ptr<TVolume> volume);
    TError Register(std::shared_ptr<TVolume> volume);
    void Unregister(std::shared_ptr<TVolume> volume);
    std::shared_ptr<TVolume> Find(const TPath &path);
    std::vector<TPath> ListPaths() const;
    TError RestoreFromStorage(std::shared_ptr<TContainerHolder> Cholder);
    void Destroy();
};
