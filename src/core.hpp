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

	TPath ExePath;
	std::string RootPath;
	std::string ExeName;

	std::string Container;
	std::string CoreCommand;
	std::string User;
	std::string Group;
	std::string Cwd;
	std::string OwnerUser;
	std::string OwnerGroup;
	std::string State;

	uid_t OwnerUid = -1;
	gid_t OwnerGid = -1;
	std::string Prefix;
	std::string Slot;

	Porto::Connection Conn;

	static TError Register(const TPath &portod);
	static TError Unregister();

	TError Handle(const TTuple &args);
	TError Identify();
	TError Forward();
	TError Save();
};
