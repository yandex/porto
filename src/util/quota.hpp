#pragma once

#include "util/error.hpp"
#include "util/path.hpp"

#include <sys/quota.h>
#include <unordered_set>
#include <unordered_map>

class TProjectQuota {
    static constexpr const char * PROJECT_QUOTA_FILE = "quota.project";
    static const uint32_t PROJECT_QUOTA_MAGIC = 0xd9c03f14;

    TPath Device;
    TPath RootPath;

    bool RemoveUnusedProjects = false;

    std::unordered_set<uint32_t> Inodes;
    std::unordered_map<uint32_t, std::unique_ptr<dqblk>> Quotas;

    std::string Type;
    TError FindProject();
    TError FindDevice();

    static TError InitProjectQuotaFile(const TPath &path);
    static TError GetProjectId(const TPath &path, uint32_t &id);
    static TError SetProjectIdOne(const TPath &path, uint32_t id, bool isDir);
    static TError SetProjectIdAll(const TPath &path, uint32_t id);
    static TError InventProjectId(const TPath &path, uint32_t &id);

    bool SeenInode(const struct stat *st);
    dqblk* FindQuota(uint32_t id);
    dqblk* SearchQuota(uint32_t id);
    TError WalkQuotaFile(int fd, unsigned id, int index, int depth);
    TError ScanQuotaFile(const TPath &quotaPath);
    TError WalkInodes(const TPathWalk &walk);
    TError UpdateQuota(uint32_t id, const dqblk *quota, std::string &message);
    TError WalkUnlinked();
    TError RecalcUsage();

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
    TError Check(std::string &message);

    TError StatFS(TStatFS &result);

    static TError Toggle(const TFile &dir, bool enabled);
};
