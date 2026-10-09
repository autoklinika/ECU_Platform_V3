#pragma once

#include "ecu/core_v2/transport/can_types.hpp"

#include <cstdint>

namespace ecu::platform::linux::v2 {

enum class SocketCanLinkQueryStatus : std::uint8_t {
  ok,
  not_found,
  not_can,
  timeout,
  io_error,
};

struct SocketCanLinkInfo {
  bool up{false};
  bool bus_off{false};
  std::uint32_t nominal_bitrate{0U};
  std::uint32_t data_bitrate{0U};
  bool fd_enabled{false};
  bool listen_only_enabled{false};
  ecu::core::v2::transport::CanCapabilities capabilities{};
};

struct SocketCanLinkQueryResult {
  SocketCanLinkQueryStatus status{
      SocketCanLinkQueryStatus::io_error};
  SocketCanLinkInfo info{};
};

[[nodiscard]] SocketCanLinkQueryResult query_socketcan_link(
    const char* interface_name,
    int timeout_ms = 50) noexcept;

}  // namespace ecu::platform::linux::v2
