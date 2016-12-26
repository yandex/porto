#include "volume.hpp"

constexpr const char *STORAGE_PRIVATE_PREFIX = "_private_";
constexpr const char *STORAGE_REMOVE_PREFIX = "_remove_";

TError SetStoragePrivate(const std::string name, const TPath &place,
                         const std::string &value);
TError GetStoragePrivate(const std::string name, const TPath &place,
                         std::string &value);
TError ClearStorage(const TPath storage);
TError RemoveStorage(const std::string name, const TPath &place);
