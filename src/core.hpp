#pragma once

#include "common.hpp"
#include "util/path.hpp"
#include "libporto.hpp"

struct TCore {
	pid_t Pid;
	pid_t Tid;
	pid_t Vpid;
	pid_t Vtid;
	int Signal;
	int Dumpable;
	uint64_t Ulimit;
	TPath DefaultPattern;
	TPath Pattern;

	std::string ProcessName;
	std::string ThreadName;

	std::string Container;
	std::string CoreCommand;
	std::string User;
	std::string Group;
	std::string Cwd;
	std::string OwnerUser;
	std::string OwnerGroup;

	uid_t OwnerUid;
	gid_t OwnerGid;
	std::string Slot;

	Porto::Connection Conn;

	static TError Register(const TPath &portod);
	static TError Unregister();

	TError Handle(const TTuple &args);
	TError Identify();
	TError Forward();
	TError Save();
};
