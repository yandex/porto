#pragma once

#include "value.hpp"

class TContainer;

constexpr const char *D_PORTO_STAT = "porto_stat";

void RegisterData(std::shared_ptr<TRawValueMap> m,
                  std::shared_ptr<TContainer> c);
