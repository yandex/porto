#pragma once

#include "util/path.hpp"

constexpr const char *LAYER_TMP_PREFIX = "_tmp_";
constexpr const char *LAYER_IMPORT_PREFIX = "_import_";
constexpr const char *LAYER_REMOVE_PREFIX = "_remove_";
constexpr const char *LAYER_PRIVATE_PREFIX = "_private_";

extern TError CheckPlace(const TPath &place, bool init = false);
extern bool LayerInUse(const std::string &name, const TPath &place);
extern TError LayerOwner(const std::string &name, const TPath &place, TCred &owner);
extern uint64_t LayerLastUsage(const std::string &name, const TPath &place);
extern bool LayerIsJunk(const std::string &name);
extern TError ImportLayer(const std::string &name, const TPath &place,
			  const TPath &tarball, bool merge,
			  const std::string private_value,
			  const TCred &owner);
extern TError RemoveLayer(const std::string &name, const TPath &place);
extern TError GetLayerPrivate(const std::string &name, const TPath &place,
                              std::string &private_value);
extern TError SetLayerPrivate(const std::string &name, const TPath &place,
                              const std::string private_value);
extern TError ValidateLayerName(const std::string &name);
extern TError SanitizeLayer(TPath layer, bool merge);
