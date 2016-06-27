#pragma once

#include "value.hpp"

class TContainer;

constexpr const char *D_EXIT_STATUS = "exit_status";
constexpr const char *D_START_ERRNO = "start_errno";
constexpr const char *D_MAX_RSS = "max_rss";
constexpr const char *D_STDOUT = "stdout";
constexpr const char *D_STDERR = "stderr";
constexpr const char *D_STDOUT_OFFSET = "stdout_offset";
constexpr const char *D_STDERR_OFFSET = "stderr_offset";
constexpr const char *D_CPU_USAGE = "cpu_usage";
constexpr const char *D_CPU_SYSTEM = "cpu_usage_system";
constexpr const char *D_MEMORY_USAGE = "memory_usage";
constexpr const char *D_ANON_USAGE = "anon_usage";
constexpr const char *D_NET_BYTES = "net_bytes";
constexpr const char *D_NET_PACKETS = "net_packets";
constexpr const char *D_NET_DROPS = "net_drops";
constexpr const char *D_NET_OVERLIMITS = "net_overlimits";
constexpr const char *D_NET_RX_BYTES = "net_rx_bytes";
constexpr const char *D_NET_RX_PACKETS = "net_rx_packets";
constexpr const char *D_NET_RX_DROPS = "net_rx_drops";
constexpr const char *D_MINOR_FAULTS = "minor_faults";
constexpr const char *D_MAJOR_FAULTS = "major_faults";
constexpr const char *D_IO_READ = "io_read";
constexpr const char *D_IO_WRITE = "io_write";
constexpr const char *D_IO_OPS = "io_ops";
constexpr const char *D_TIME = "time";
constexpr const char *D_PORTO_STAT = "porto_stat";

void RegisterData(std::shared_ptr<TRawValueMap> m,
                  std::shared_ptr<TContainer> c);
