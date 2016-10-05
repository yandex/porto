#pragma once

#include <memory>
#include <string>
#include "common.hpp"
#include "statistics.hpp"
#include "util/path.hpp"
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

constexpr const char *V_PLACE = "place";

class TVolume;
class TContainer;
class TKeyValue;

class TVolumeBackend {
public:
    TVolume *Volume;
    virtual TError Configure(void);
    virtual TError Save(void);
    virtual TError Restore(void);
    virtual TError Build(void) =0;
    virtual TError Destroy(void) =0;
    virtual TError StatFS(TStatFS &result) =0;
    virtual TError Clear(void);
    virtual TError Resize(uint64_t space_limit, uint64_t inode_limit);
};

class TVolume : public std::enable_shared_from_this<TVolume>,
                public TLockable,
                public TNonCopyable {

    std::unique_ptr<TVolumeBackend> Backend;
    TError OpenBackend();

public:
    TPath Path;
    bool IsAutoPath = false;
    std::string Storage;
    TPath StorageFile;
    std::string BackendType;
    std::string Creator;
    std::string Id;
    bool IsReady = false;
    bool IsDying = false;
    std::string Private;
    std::vector<std::string> Containers;
    int LoopDev = -1;
    bool IsReadOnly = false;
    std::vector<std::string> Layers;
    uint64_t SpaceLimit = 0;
    uint64_t SpaceGuarantee = 0;
    uint64_t InodeLimit = 0;
    uint64_t InodeGuarantee = 0;
    TCred VolumeOwner;
    TCred CreatorCred;
    TPath CreatorRoot;
    unsigned VolumePerms = 0775;

    bool CustomPlace = false;
    TPath Place;

    TVolume() {
        Statistics->VolumesCount++;
    }
    ~TVolume() {
        Statistics->VolumesCount--;
    }

    static TError Create(const TPath &path, const TStringMap &cfg,
                         TContainer &container, const TCred &cred,
                         std::shared_ptr<TVolume> &volume);

    static std::shared_ptr<TVolume> Find(const TPath &path);
    static TError Find(const TPath &path, std::shared_ptr<TVolume> &volume);

    TError Configure(const TPath &path, const TStringMap &cfg,
                     const TContainer &container, const TCred &cred);
    TError ApplyConfig(const TStringMap &cfg);
    TStringMap DumpState(const TPath &root);

    TError Build(void);
    TError DestroyOne(void);
    TError Destroy(void);
    TError Clear(void);

    TError Save(void);
    TError Restore(const TKeyValue &node);

    static void RestoreAll(void);

    TError LinkContainer(TContainer &container);
    TError UnlinkContainer(TContainer &container);

    TPath GetStorage(void) const;
    TPath GetInternal(std::string type) const;
    unsigned long GetMountFlags(void) const;

    TError Tune(const TStringMap &cfg);

    TError Resize(uint64_t space_limit, uint64_t inode_limit);

    TError CheckGuarantee(uint64_t space_guarantee, uint64_t inode_guarantee) const;

    bool HaveQuota(void) const {
        return SpaceLimit || InodeLimit;
    }

    bool HaveStorage(void) const {
        return Storage.size();
    }

    bool HaveLayers(void) const {
        return !Layers.empty();
    }

    TError StatFS(TStatFS &result) const;

    TError GetUpperLayer(TPath &upper);
};

struct TVolumeProperty {
    std::string Name;
    std::string Desc;
    bool ReadOnly;
};

extern std::vector<TVolumeProperty> VolumeProperties;

extern std::mutex VolumesMutex;
extern std::map<TPath, std::shared_ptr<TVolume>> Volumes;
extern TPath VolumesKV;

static inline std::unique_lock<std::mutex> LockVolumes() {
    return std::unique_lock<std::mutex>(VolumesMutex);
}
