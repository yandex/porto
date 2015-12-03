#pragma once

#include "error.hpp"
#include "util/path.hpp"

class TProjectQuota {
	static constexpr const char * PROJECT_QUOTA_FILE = "quota.project";
	static const uint32_t PROJECT_QUOTA_MAGIC = 0xd9c03f14;

	TPath Device;
	TPath RootPath;

	TError FindDevice();
	TError EnableProjectQuota();

	static TError InitProjectQuotaFile(TPath path);
	static TError GetProjectId(const TPath &path, uint32_t &id);
	static TError SetProjectIdOne(const char *path, uint32_t id);
	static TError SetProjectIdAll(const TPath &path, uint32_t id);
public:
	TPath Path;

	uint32_t ProjectId;
	uint64_t SpaceLimit;
	uint64_t SpaceUsage;
	uint64_t InodeLimit;
	uint64_t InodeUsage;

	TProjectQuota(const TPath &path) { Path = path; }

	bool Supported();
	bool Exists();

	TError Load();
	TError Create();
	TError Resize();
	TError Destroy();
};
