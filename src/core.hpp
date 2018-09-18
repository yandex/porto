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

	TString ProcessName;
	TString ThreadName;

	TPath ExePath;
	TString RootPath;
	TString ExeName;

	TString Container;
	TString CoreCommand;
	TString User;
	TString Group;
	TString Cwd;
	TString OwnerUser;
	TString OwnerGroup;

	uid_t OwnerUid = -1;
	gid_t OwnerGid = -1;
	TString Prefix;
	TString Slot;

	Porto::Connection Conn;

	static TError Register(const TPath &portod);
	static TError Unregister();

	TError Handle(const TTuple &args);
	TError Identify();
	TError Forward();
	TError Save();
};
