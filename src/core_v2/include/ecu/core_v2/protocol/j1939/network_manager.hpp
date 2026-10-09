#pragma once

#include "ecu/core_v2/protocol/j1939/address_claim.hpp"
#include "ecu/core_v2/protocol/j1939/transport_protocol.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

enum class NetworkManagerStatus : std::uint8_t {
  ok,
  no_action,
  invalid_state,
  queue_overflow,
  protocol_fault,
};

struct CommandedAddressMessage {
  std::uint64_t target_name{0U};
  std::uint8_t new_address{kNullAddress};
  std::uint8_t source_address{kNullAddress};
};

[[nodiscard]] bool decode_commanded_address_message(
    const TpMessage& message,
    CommandedAddressMessage& value) noexcept;

struct NetworkManagerCounters {
  std::uint32_t frames_seen{0U};
  std::uint32_t malformed_management_frames{0U};
  std::uint32_t address_conflicts{0U};
  std::uint32_t address_losses{0U};
  std::uint32_t commanded_address_messages{0U};
  std::uint32_t commanded_address_applied{0U};
  std::uint32_t tx_enqueued{0U};
  std::uint32_t tx_queue_overflows{0U};
};

class NetworkManager final : public transport::ICanFrameSink {
 public:
  static constexpr std::size_t kTxQueueCapacity = 8U;

  [[nodiscard]] bool configure(
      const AddressClaimConfig& config) noexcept;

  [[nodiscard]] NetworkManagerStatus start(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] NetworkManagerStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] NetworkManagerStatus on_transport_message(
      const TpMessage& message,
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;

  [[nodiscard]] AddressClaimState address_claim_state()
      const noexcept;
  [[nodiscard]] std::uint8_t current_address() const noexcept;
  [[nodiscard]] NetworkManagerStatus status() const noexcept;
  [[nodiscard]] NetworkManagerCounters counters() const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;

  [[nodiscard]] static transport::CanFilter rx_filter() noexcept;

 private:
  void handle_step(const AddressClaimStep& step) noexcept;
  [[nodiscard]] bool enqueue(
      const transport::CanFrame& frame) noexcept;
  void latch_protocol_fault() noexcept;

  AddressClaimEngine address_claim_{};
  std::array<transport::CanFrame, kTxQueueCapacity> tx_queue_{};
  std::size_t tx_head_{0U};
  std::size_t tx_tail_{0U};
  std::size_t tx_count_{0U};
  NetworkManagerCounters counters_{};
  NetworkManagerStatus status_{NetworkManagerStatus::invalid_state};
  bool configured_{false};
};

}  // namespace ecu::core::v2::protocol::j1939
