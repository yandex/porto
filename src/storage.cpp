#include "storage.hpp"
#include "helpers.hpp"
#include "util/path.hpp"
#include "util/log.hpp"

TError SetStoragePrivate(const std::string name, const TPath &place,
                         const std::string &value) {
    std::string private_name = STORAGE_PRIVATE_PREFIX + name;
    TPath storage_private = place / PORTO_STORAGE / private_name;
    TFile private_file;
    TError error;

    if (!storage_private.Exists())
        error = private_file.CreateNew(storage_private, 0600);
    else
        error = private_file.OpenTrunc(storage_private);

    if (error)
        return error;

    error = private_file.WriteAll(value);
    if (error)
        return error;

    return TError::Success();
}


TError GetStoragePrivate(const std::string name, const TPath &place,
                         std::string &value) {
    std::string private_name = STORAGE_PRIVATE_PREFIX + name;
    TPath storage_private = place / PORTO_STORAGE / private_name;
    TFile private_file;
    TError error;

    value = "";

    if (!storage_private.Exists())
        return TError::Success();

    error = private_file.OpenRead(storage_private);
    if (error)
        return error;

    error = private_file.ReadAll(value, 4096);
    if (error)
        return error;

    return TError::Success();
}

TError ClearStorage(const TPath storage) {
    TError error = ClearRecursive(storage);
    if (error)
        L_ERR() << "Cannot clear storage: " << error << std::endl;

    error = storage.RemoveAll();
    if (error)
        L_ERR() << "Can't remove storage: " << error << std::endl;

    return error;
}

TError RemoveStorage(const std::string name, const TPath &place) {
    TPath storage_private = place / PORTO_STORAGE / (STORAGE_PRIVATE_PREFIX + name);
    TPath storage = place / PORTO_STORAGE / name;
    TPath tmp = place / PORTO_STORAGE / (STORAGE_REMOVE_PREFIX + name);
    TError error;

    auto lock = LockVolumes();

    if (!storage.Exists())
        return TError(EError::InvalidValue, "Storage path does not exists");

    for (auto &i : Volumes) {
        auto v = i.second;
        if (v->GetStorage() == storage && place == v->Place)
            return TError(EError::Busy, "Storage is used by volume : " + v->Path.ToString());
    }

    error = storage.Rename(tmp);
    if (error)
        return TError(EError::Unknown, "Cannot rename storage "
                                       "marked for removal");
    if (storage_private.Exists()) {
        error = storage_private.Unlink();
        if (error)
            L_WRN() << "Cannot remove storage private file: " << error << std::endl;
    }

    lock.unlock();

    return ClearStorage(tmp);
}
