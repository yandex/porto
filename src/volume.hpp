#pragma once

#include <memory>
#include <string>
#include <set>
#include <mutex>
#include "common.hpp"
#include "statistics.hpp"
#include "util/path.hpp"

constexpr const char *V_ID = "id";
constexpr const char *V_PATH = "path";
constexpr const char *V_BACKEND = "backend";
constexpr const char *V_READY = "ready";
constexpr const char *V_BUILD_TIME = "build_time";
constexpr const char *V_STATE = "state";
constexpr const char *V_PRIVATE = "private";

constexpr const char *V_RAW_ID = "_id";
constexpr const char *V_RAW_CONTAINERS = "_containers";
constexpr const char *V_CONTAINERS = "containers";
constexpr const char *V_LOOP_DEV = "_loop_dev";
constexpr const char *V_AUTO_PATH = "_auto_path";
constexpr const char *V_TARGET_CONTAINER = "target_container";

constexpr const char *V_OWNER_CONTAINER = "owner_container";
constexpr const char *V_OWNER_USER = "owner_user";
constexpr const char *V_OWNER_GROUP = "owner_group";
constexpr const char *V_CREATOR = "creator";

constexpr const char *V_USER = "user";
constexpr const char *V_GROUP = "group";
constexpr const char *V_PERMISSIONS = "permissions";

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
constexpr const char *V_PLACE_KEY = "place_key";

class TVolume;
class TContainer;
class TKeyValue;

enum class EVolumeState {
    Initial,
    Building,
    Ready,
    Unlinked,
    ToDestroy,
    Destroying,
    Destroyed,
};

class TVolumeBackend {
public:
    TVolume *Volume;
    virtual TError Configure(void);
    virtual TError Restore(void);
    virtual TError Build(void) =0;
    virtual TError Destroy(void) =0;
    virtual TError StatFS(TStatFS &result) =0;
    virtual TError Resize(uint64_t space_limit, uint64_t inode_limit);
    virtual std::string ClaimPlace();
};

class TVolumeLink {
public:
    std::shared_ptr<TVolume> Volume;
    std::shared_ptr<TContainer> Container;
    TPath Target;           /* path in container namespace */
    TPath HostTarget;       /* mounted path in host namespace */
    bool ReadOnly = false;
    bool Required = false;

    TVolumeLink(std::shared_ptr<TVolume> v, std::shared_ptr<TContainer> c) : Volume(v), Container(c) {}
};

class TVolume : public std::enable_shared_from_this<TVolume>,
                public TNonCopyable {

    std::unique_ptr<TVolumeBackend> Backend;
    TError OpenBackend();

    std::mutex Mutex;

public:
    TPath Path;
    TPath InternalPath;
    bool IsAutoPath = false;
    std::string BuildTime;

    TPath Place = PORTO_PLACE;
    bool CustomPlace = false;

    std::string Storage;
    TPath StoragePath;
    TFile StorageFd; /* during build */
    bool KeepStorage = false;

    std::string BackendType;
    std::string Id;

    EVolumeState State = EVolumeState::Initial;
    static std::string StateName(EVolumeState state);
    void SetState(EVolumeState state);

    int Device = -1;
    bool IsReadOnly = false;

    bool HasDependentContainer = false;

    std::vector<std::string> Layers;
    std::list<std::shared_ptr<TVolumeLink>> Links;

    uint64_t ClaimedSpace = 0;
    uint64_t SpaceLimit = 0;
    uint64_t SpaceGuarantee = 0;
    uint64_t InodeLimit = 0;
    uint64_t InodeGuarantee = 0;

    /* protected with VolumesLock */
    std::shared_ptr<TContainer> VolumeOwnerContainer;

    TCred VolumeOwner;
    TCred VolumeCred;
    unsigned VolumePerms = 0;

    std::string Creator;
    std::string Private;

    std::set<std::shared_ptr<TVolume>> Nested;

    TVolume() {
        Statistics->VolumesCount++;
    }
    ~TVolume() {
        Statistics->VolumesCount--;
    }

    std::unique_lock<std::mutex> LockState() {
        return std::unique_lock<std::mutex>(Mutex);
    }

    static TError Create(const TStringMap &cfg,
                         std::shared_ptr<TVolume> &volume);

    /* link target path */
    static std::shared_ptr<TVolume> ResolveMountLocked(const TPath &path);
    static std::shared_ptr<TVolume> ResolveMount(const TPath &path);

    /* link inner path */
    static std::shared_ptr<TVolume> ResolveOriginLocked(const TPath &path);
    static std::shared_ptr<TVolume> ResolveOrigin(const TPath &path);

    TPath ComposePath(const TContainer &ct) const;

    TError Configure(const TPath &target_root, const TStringMap &cfg);
    TError ApplyConfig(const TStringMap &cfg);

    void Dump(const TPath &path, rpc::TVolumeDescription *dump);

    TError DependsOn(const TPath &path);
    TError CheckDependencies();
    static TError CheckConflicts(const TPath &path);

    TError Build(void);

    static void DestroyAll();
    TError DestroyOne();
    TError Destroy();

    TError Save(void);
    TError Restore(const TKeyValue &node);

    static void RestoreAll(void);

    TError MountLink(TVolumeLink &link);

    TError UmountLink(TVolumeLink &link, bool strict = false);

    TError LinkVolume(std::shared_ptr<TContainer> container,
                      const TPath &target = "",
                      bool read_only = false,
                      bool required = false);

    TError UnlinkVolume(std::shared_ptr<TContainer> container,
                        const TPath &target,
                        bool strict = false);

    static void UnlinkAllVolumes(std::shared_ptr<TContainer> container);

    static TError CheckRequired(const std::list<std::string> &paths);

    TError ClaimPlace(uint64_t size);

    TPath GetInternal(const std::string &type) const;
    unsigned long GetMountFlags(void) const;

    TError Tune(const TStringMap &cfg);

    TError CheckGuarantee(uint64_t space_guarantee, uint64_t inode_guarantee);

    bool HaveQuota(void) const {
        return SpaceLimit || InodeLimit;
    }

    bool HaveStorage(void) const {
        return !Storage.empty();
    }

    /* User provides directory for storage */
    bool UserStorage(void) const {
        return Storage[0] == '/';
    }

    /* They do not keep data in StoragePath */
    bool RemoteStorage(void) const {
        return BackendType == "rbd" ||
               BackendType == "lvm" ||
               BackendType == "tmpfs" ||
               BackendType == "dir" ||
               BackendType == "quota";
    }

    /* Backend storage could be a regular file */
    bool FileStorage(void) const {
        return BackendType == "loop";
    }

    bool HaveLayers(void) const {
        return !Layers.empty();
    }

    TError StatFS(TStatFS &result);

    TError GetUpperLayer(TPath &upper);

    friend bool operator<(const std::shared_ptr<TVolume> &lhs,
                          const std::shared_ptr<TVolume> &rhs) {
        return lhs->Path < rhs->Path;
    }
};

struct TVolumeProperty {
    std::string Name;
    std::string Desc;
    bool ReadOnly;
};

extern std::vector<TVolumeProperty> VolumeProperties;

extern std::mutex VolumesMutex;
extern std::map<TPath, std::shared_ptr<TVolume>> Volumes;
extern std::map<TPath, std::shared_ptr<TVolume>> VolumeMounts;
extern TPath VolumesKV;

static inline std::unique_lock<std::mutex> LockVolumes() {
    return std::unique_lock<std::mutex>(VolumesMutex);
}

extern TError PutLoopDev(const int loopNr); /* Legacy */
