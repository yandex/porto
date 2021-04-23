#pragma once

#include "util/error.hpp"
#include "util/path.hpp"

class TProjectQuota {
    static constexpr const char * PROJECT_QUOTA_FILE = "quota.project";
    static const uint32_t PROJECT_QUOTA_MAGIC = 0xd9c03f14;

    TPath Device;
    TPath RootPath;
    std::string Type;

    TError FindProject();
    TError FindDevice();

    static TError InitProjectQuotaFile(const TPath &path);
    static TError GetProjectId(const TPath &path, uint32_t &id);
    static TError SetProjectIdOne(const TPath &path, uint32_t id, bool isDir);
    static TError SetProjectIdAll(const TPath &path, uint32_t id);
    static TError InventProjectId(const TPath &path, uint32_t &id);
public:
    TPath Path;

    uint32_t ProjectId = 0;
    uint64_t SpaceLimit = 0;
    uint64_t SpaceUsage = 0;
    uint64_t InodeLimit = 0;
    uint64_t InodeUsage = 0;

    TProjectQuota(const TPath &path) { Path = path; }
    TError Enable();

    bool Exists();

    TError Load();
    TError Create();
    TError Resize();
    TError Destroy();

    TError StatFS(TStatFS &result);

    static TError Toggle(const TFile &dir, bool enabled);
};
