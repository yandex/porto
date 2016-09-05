#pragma once

#include "util/path.hpp"

extern TError CheckPlace(const TPath &place, bool init = false);
extern bool LayerInUse(const std::string &name, const TPath &place);
extern TError ImportLayer(const std::string &name, const TPath &place,
			  const TPath &tarball, bool merge);
extern TError RemoveLayer(const std::string &name, const TPath &place);
extern TError ValidateLayerName(const std::string &name);
extern TError SanitizeLayer(TPath layer, bool merge);
