#pragma once

#include <list>
#include "util/path.hpp"

class TStorage {
public:
    TPath Path;
    TPath Place;
    std::string Type;
    std::string Name;

    TCred Owner;
    std::string Private;
    time_t LastChange = 0;

    TStorage(const TPath &place, const std::string &type,
             const std::string &name) :
        Path(place / type / name), Place(place), Type(type), Name(name) {}

    static TError CheckName(const std::string &name);
    static TError CheckPlace(const TPath &place);
    static TError SanitizeLayer(const TPath &layer, bool merge);
    static TError List(const TPath &place, const std::string &type,
                       std::list<TStorage> &list);
    TError ImportArchive(const TPath &archive, const std::string &compress = "", bool merge = false);
    TError ExportArchive(const TPath &archive, const std::string &compress = "");
    bool Exists();
    uint64_t LastUsage();
    TError Load();
    TError Remove();
    TError Touch();
    TError SetOwner(const TCred &owner);
    TError SetPrivate(const std::string &text);

    static void Init();
    static void IncPlaceLoad(const TPath &place);
    static void DecPlaceLoad(const TPath &place);

private:
    static TError Cleanup(const TPath &place, const std::string &type, unsigned perms);
    TPath TempPath(const std::string &kind);
    TError CheckUsage();
};
