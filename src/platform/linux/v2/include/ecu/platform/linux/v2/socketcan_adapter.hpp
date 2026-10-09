#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/i_can_driver.hpp"

#include <array>
#include <cstdint>

#include <net/if.h>

namespace ecu::platform::linux::v2 {

class SocketCanAdapter final
    : public ecu::core::v2::transport::ICanDriver {
 public:
  SocketCanAdapter(
      const char* interface_name,
      const ecu::core::v2::time::IMonotonicClock& clock,
      ecu::core::v2::transport::CanDriverExecutionContract
          execution_contract) noexcept;

  ~SocketCanAdapter() override;

  SocketCanAdapter(const SocketCanAdapter&) = delete;
  SocketCanAdapter& operator=(const SocketCanAdapter&) = delete;
  SocketCanAdapter(SocketCanAdapter&&) = delete;
  SocketCanAdapter& operator=(SocketCanAdapter&&) = delete;

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] ecu::core::v2::transport::CanPhysicalChannelId
  physical_channel_id() const noexcept override;

  [[nodiscard]] ecu::core::v2::transport::ICanChannelArbiter&
  channel_arbiter() noexcept override;

  [[nodiscard]] ecu::core::v2::transport::CanDriverExecutionContract
  execution_contract() const noexcept override;

  [[nodiscard]] ecu::core::v2::transport::CanCapabilities
  capabilities() const noexcept override;

  [[nodiscard]] ecu::core::v2::transport::CanStatus open(
      const ecu::core::v2::transport::CanChannelConfig& config)
      noexcept override;

  void close() noexcept override;
  [[nodiscard]] bool is_open() const noexcept override;

  [[nodiscard]] ecu::core::v2::transport::CanStatus try_send(
      const ecu::core::v2::transport::CanFrame& frame)
      noexcept override;

  [[nodiscard]] ecu::core::v2::transport::CanReceiveResult
  try_receive() noexcept override;

 private:
  std::array<char, IFNAMSIZ> interface_name_{};
  const ecu::core::v2::time::IMonotonicClock& clock_;
  ecu::core::v2::time::MonotonicClockProperties clock_properties_{};
  ecu::core::v2::transport::CanPhysicalChannelId channel_id_{};
  ecu::core::v2::transport::CanDriverExecutionContract execution_{};
  ecu::core::v2::transport::CanCapabilities capabilities_{};
  ecu::core::v2::transport::CanCapabilities active_capabilities_{};
  ecu::core::v2::transport::CanChannelConfig active_config_{};
  int socket_fd_{-1};
  std::uint32_t rx_drop_total_{0U};
  bool valid_{false};
};

}  // namespace ecu::platform::linux::v2
