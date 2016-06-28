#pragma once

#include "value.hpp"

class TContainer;

constexpr const char *D_IO_READ = "io_read";
constexpr const char *D_IO_WRITE = "io_write";
constexpr const char *D_IO_OPS = "io_ops";
constexpr const char *D_TIME = "time";
constexpr const char *D_PORTO_STAT = "porto_stat";

void RegisterData(std::shared_ptr<TRawValueMap> m,
                  std::shared_ptr<TContainer> c);
