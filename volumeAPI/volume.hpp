#ifndef __VOLUME_H__
#define __VOLUME_H__

#include <string>
#include <set>

#include "kvalue.hpp"
#include "common.hpp"
#include "util/mount.hpp"
#include "util/cred.hpp"

class TVolumeHolder;

class TVolume : public std::enable_shared_from_this<TVolume>, public TNonCopyable {
public:
    TError Create();
    TError Construct() const;
    TError Deconstruct() const;
    TError Destroy();
    TVolume(std::shared_ptr<TKeyValueStorage> storage,
            std::shared_ptr<TVolumeHolder> holder, const std::string &name,
            const std::string &source, const std::string &quota,
            const std::string &flags, const TCred &cred) :
        Storage(storage), Holder(holder), Cred(cred), Name(name),
        Source(source), Quota(quota), Flags(flags) {
    }
    TVolume(std::shared_ptr<TKeyValueStorage> storage,
            std::shared_ptr<TVolumeHolder> holder, const std::string &name) :
        Storage(storage), Holder(holder), Name(name) {
    }

    TError CheckPermission(const TCred &ucred) const;

    const std::string &GetName() const { return Name; }
    const std::string &GetSource() const { return Source; }
    const std::string &GetQuota() const { return Quota; }
    const std::string &GetFlags() const { return Flags; }

    TError SaveToStorage() const;
    TError LoadFromStorage();
private:
    std::shared_ptr<TKeyValueStorage> Storage;
    std::shared_ptr<TVolumeHolder> Holder;
    TCred Cred;

    std::string Name;
    std::string Source;
    std::string Quota;
    uint64_t ParsedQuota;
    std::string Flags;

    int LoopDev = -1;
    TPath GetLoopPath() const;
};

class TVolumeHolder : public TNonCopyable, public std::enable_shared_from_this<TVolumeHolder> {
public:
    TError Insert(std::shared_ptr<TVolume> volume);
    void Remove(std::shared_ptr<TVolume> volume);
    std::shared_ptr<TVolume> Get(const std::string &name);
    std::vector<std::string> List() const;
    TVolumeHolder(std::shared_ptr<TKeyValueStorage> storage) :
        Storage(storage) {}
    TError RestoreFromStorage();
private:
    std::shared_ptr<TKeyValueStorage> Storage;
    std::map<std::string, std::shared_ptr<TVolume> > Volumes;
};

#endif /* __VOLUME_H__ */
