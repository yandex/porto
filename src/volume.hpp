#pragma once

#include <string>

#include "common.hpp"
#include "statistics.hpp"
#include "util/mount.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"
#include "util/locks.hpp"

constexpr const char *V_PATH = "path";
constexpr const char *V_BACKEND = "backend";
constexpr const char *V_READY = "ready";
constexpr const char *V_PRIVATE = "private";

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
class TKeyValue;

TError ValidateLayerName(const std::string &name);

TError SanitizeLayer(TPath layer, bool merge);

class TVolumeBackend {
public:
    TVolume *Volume;
    virtual TError Configure();
    virtual TError Save();
    virtual TError Restore();
    virtual TError Build() =0;
    virtual TError Destroy() =0;
    virtual TError StatFS(TStatFS &result) =0;
    virtual TError Clear();
    virtual TError Resize(uint64_t space_limit, uint64_t inode_limit);
};

class TVolume : public std::enable_shared_from_this<TVolume>,
                public TLockable,
                public TNonCopyable {
    friend class TVolumeHolder;

    std::unique_ptr<TVolumeBackend> Backend;
    TError OpenBackend();

public:
    std::string Path;
    bool IsAutoPath = false;
    std::string StoragePath;
    bool IsAutoStorage = true;
    std::string BackendType;
    std::string Creator;
    std::string Id;
    bool IsReady = false;
    std::string Private;
    std::vector<std::string> Containers;
    int LoopDev = -1;
    bool IsReadOnly = false;
    std::vector<std::string> Layers;
    bool IsLayersSet = false;
    uint64_t SpaceLimit = 0;
    uint64_t SpaceGuarantee = 0;
    uint64_t InodeLimit = 0;
    uint64_t InodeGuarantee = 0;
    TCred VolumeOwner;
    unsigned VolumePerms = 0755;

    TVolume() {
        Statistics->Volumes++;
    }
    ~TVolume() {
        Statistics->Volumes--;
    }
    TError Configure(const TPath &path, const TCred &creator_cred,
                     std::shared_ptr<TContainer> creator_container,
                     const std::map<std::string, std::string> &properties,
                     TVolumeHolder &holder);

    /* Protected with TVolume->Lock() */
    TError Build();
    TError Destroy(TVolumeHolder &holder);

    TError Save();
    TError Restore(const TKeyValue &node);
    TError Clear();

    const std::vector<std::string> GetContainers() const {
        return Containers;
    }
    TError LinkContainer(const std::string name);
    bool UnlinkContainer(const std::string name);

    TError CheckPermission(const TCred &ucred) const;

    TPath GetPath() const;
    TPath GetStorage() const;
    TPath GetInternal(std::string type) const;
    TPath GetChrootInternal(TPath container_root, std::string type) const;
    unsigned long GetMountFlags() const;

    /* Protected with TVolume->Lock() _and_ TVolumeHolder->Lock() */
    TError SetReady(bool ready) { IsReady = ready; return Save(); }

    TError Tune(TVolumeHolder &holder,
            const std::map<std::string, std::string> &properties);

    TError Resize(uint64_t space_limit, uint64_t inode_limit);

    TError CheckGuarantee(TVolumeHolder &holder,
            uint64_t space_guarantee, uint64_t inode_guarantee) const;

    bool HaveQuota() const {
        return SpaceLimit || InodeLimit;
    }

    void GetQuota(uint64_t &space_limit, uint64_t &inode_limit) const {
        space_limit = SpaceLimit;
        inode_limit = InodeLimit;
    }

    TError StatFS(TStatFS &result) const;

    TError GetUpperLayer(TPath &upper);

    std::vector<TPath> GetLayers() const;

    TError SetProperty(const std::map<std::string, std::string> &properties);

    std::map<std::string, std::string> GetProperties(TPath container_root);
};

struct TVolumeProperty {
    std::string Name;
    std::string Desc;
    bool ReadOnly;
};

extern std::vector<TVolumeProperty> VolumeProperties;

class TVolumeHolder : public std::enable_shared_from_this<TVolumeHolder>,
                      public TLockable,
                      public TNonCopyable {
    std::map<TPath, std::shared_ptr<TVolume>> Volumes;
    uint64_t NextId = 1;
public:
    TVolumeHolder() {}
    TError Create(std::shared_ptr<TVolume> &volume);
    void Remove(std::shared_ptr<TVolume> volume);
    TError Register(std::shared_ptr<TVolume> volume);
    void Unregister(std::shared_ptr<TVolume> volume);
    std::shared_ptr<TVolume> Find(const TPath &path);
    std::vector<TPath> ListPaths() const;
    TError RestoreFromStorage(std::shared_ptr<TContainerHolder> Cholder);
    void Destroy();

    bool LayerInUse(TPath layer);
    TError RemoveLayer(const std::string &name);
};

extern TPath VolumesKV;
