#pragma once

#include <string>
#include <set>

#include "kvalue.hpp"
#include "common.hpp"
#include "util/mount.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"

class TVolumeHolder;
class TVolume;
class TResource;

class TVolumeImpl {
protected:
    std::shared_ptr<TVolume> Volume;
public:
    TVolumeImpl(std::shared_ptr<TVolume> volume) : Volume(volume) {}
    virtual TError Create() =0;
    virtual TError Destroy() =0;
    virtual bool Save(kv::TNode &node) =0;
    virtual void Restore(kv::TNode &node) =0;
    virtual TError Construct() const =0;
    virtual TError Deconstruct() const =0;
};

class TVolume : public std::enable_shared_from_this<TVolume>, public TNonCopyable {
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TVolumeHolder> Holder;
    TCred Cred;

    TPath Path;
    std::shared_ptr<TResource> Resource;
    std::string Quota;
    uint64_t ParsedQuota;
    std::string Flags;
    uint16_t Id;

    std::unique_ptr<TVolumeImpl> Impl;
    TError Prepare();
public:
    TError Create();
    TError Construct() const;
    TError Deconstruct() const;
    TError Destroy();
    TVolume(std::shared_ptr<TKeyValueStorage> storage,
            std::shared_ptr<TVolumeHolder> holder, const TPath &path,
            std::shared_ptr<TResource> resource,
            const std::string &quota,
            const std::string &flags, const TCred &cred) :
        Storage(storage), Holder(holder), Cred(cred), Path(path),
        Resource(resource), Quota(quota), Flags(flags) {}
    TVolume(std::shared_ptr<TKeyValueStorage> storage,
            std::shared_ptr<TVolumeHolder> holder) :
        Storage(storage), Holder(holder) {}

    TError CheckPermission(const TCred &ucred) const;

    const TPath &GetPath() const { return Path; }
    const std::string GetSource() const;
    const std::string &GetQuota() const { return Quota; }
    const uint64_t GetParsedQuota() const { return ParsedQuota; }
    const std::string &GetFlags() const { return Flags; }
    const uint16_t GetId() const { return Id; }
    std::shared_ptr<TResource> GetResource() { return Resource; }

    TError SaveToStorage(const std::string &path) const;
    TError LoadFromStorage(const std::string &path);
    TCred GetCred() const { return Cred; }
};

class TResource : public TNonCopyable {
    TPath Source;
    TPath Path;
    TError Untar(const TPath &what, const TPath &where) const;
public:
    TResource(const TPath &source, const TPath &path = "") : Source(source), Path(path) {}
    ~TResource();
    TError Prepare();
    TError Create() const;
    TError Copy(const TPath &to) const;
    TError Destroy() const;
    const TPath &GetSource() { return Source; }
    const TPath &GetPath() { return Path; }
};

class TVolumeHolder : public TNonCopyable, public std::enable_shared_from_this<TVolumeHolder> {
    std::shared_ptr<TKeyValueStorage> Storage;
    std::map<TPath, std::shared_ptr<TVolume>> Volumes;
    std::map<TPath, std::weak_ptr<TResource>> Resources;
    void RemoveUnusedResources();
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
    TError GetResource(const TPath &path, std::shared_ptr<TResource> &resource);
};
