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
constexpr const char *V_SOURCE = "source";
constexpr const char *V_QUOTA = "quota";
constexpr const char *V_FLAGS = "flags";
constexpr const char *V_USER = "user";
constexpr const char *V_GROUP = "group";
constexpr const char *V_ID = "_id";
constexpr const char *V_VALID = "_valid";
constexpr const char *V_LOOP_DEV = "_loop_dev";

class TVolumeHolder;
class TVolume;

class TVolumeImpl {
protected:
    std::shared_ptr<TVolume> Volume;
public:
    TVolumeImpl(std::shared_ptr<TVolume> volume) : Volume(volume) {}
    virtual TError Create() =0;
    virtual TError Destroy() =0;
    virtual TError Save(std::shared_ptr<TValueMap> data) =0;
    virtual TError Restore(std::shared_ptr<TValueMap> data) =0;
    virtual TError Construct() const =0;
    virtual TError Deconstruct() const =0;
    virtual TError GetUsage(uint64_t &used, uint64_t &avail) const =0;
};

class TVolume : public std::enable_shared_from_this<TVolume>, public TNonCopyable {
    std::shared_ptr<TKeyValueNode> KvNode;
    std::shared_ptr<TVolumeHolder> Holder;
    std::shared_ptr<TValueMap> Data = nullptr;
    TCred Cred;
    uint64_t ParsedQuota;

    std::unique_ptr<TVolumeImpl> Impl;
    TError Prepare();
    TError ParseQuota(const std::string &quota);
public:
    TError Create(std::shared_ptr<TKeyValueStorage> storage,
                  const TPath &path,
                  const std::string &quota,
                  const std::string &flags);
    TError Construct() const;
    TError Deconstruct() const;
    TError Destroy();
    TVolume(std::shared_ptr<TVolumeHolder> holder,
            const TCred &cred) :
        KvNode(nullptr), Holder(holder), Cred(cred) {}
    TVolume(std::shared_ptr<TKeyValueNode> kvnode,
            std::shared_ptr<TVolumeHolder> holder) :
        KvNode(kvnode), Holder(holder) {}

    TError CheckPermission(const TCred &ucred) const;

    TPath GetPath() const { return Data->Get<std::string>(V_PATH); }
    std::string GetQuota() const { return Data->Get<std::string>(V_QUOTA); }
    uint64_t GetParsedQuota() const { return ParsedQuota; }
    std::string GetFlags() const { return Data->Get<std::string>(V_FLAGS); }
    uint16_t GetId() const { return (uint16_t)Data->Get<int>(V_ID); }
    TError GetUsage(uint64_t &used, uint64_t &avail) const;

    TError LoadFromStorage();
    TCred GetCred() const { return Cred; }
    bool IsValid() const;
    TError SetValid(bool v);
};

class TVolumeHolder : public TNonCopyable, public std::enable_shared_from_this<TVolumeHolder> {
    std::shared_ptr<TKeyValueStorage> Storage;
    std::map<TPath, std::shared_ptr<TVolume>> Volumes;
    void RemoveUnusedVolumes();
public:
    TIdMap IdMap;
    TError Insert(std::shared_ptr<TVolume> volume);
    void Remove(std::shared_ptr<TVolume> volume);
    std::shared_ptr<TVolume> Get(const TPath &path);
    std::vector<TPath> List() const;
    TVolumeHolder(std::shared_ptr<TKeyValueStorage> storage) :
        Storage(storage) {}
    TError RestoreFromStorage();
    void Destroy();
};
