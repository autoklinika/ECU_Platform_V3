#pragma once

#include "ecu/core_v2/protocol/j1939/network_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::isobus {

enum class ControlFunctionState : std::uint8_t {
  claimed,
  cannot_claim,
};

struct ControlFunctionRecord {
  bool in_use{false};
  std::uint64_t name{0U};
  std::uint8_t source_address{j1939::kNullAddress};
  ControlFunctionState state{ControlFunctionState::cannot_claim};
};

enum class RegistryStatus : std::uint8_t {
  ok,
  no_action,
  malformed_frame,
  capacity_exhausted,
  address_conflict,
};

enum class LookupStatus : std::uint8_t {
  found,
  not_found,
  ambiguous,
};

struct ControlFunctionRegistryCounters {
  std::uint32_t frames_seen{0U};
  std::uint32_t claims_seen{0U};
  std::uint32_t discovered{0U};
  std::uint32_t address_changes{0U};
  std::uint32_t cannot_claim_reports{0U};
  std::uint32_t address_conflicts{0U};
  std::uint32_t malformed_claims{0U};
  std::uint32_t capacity_exhaustions{0U};
};

class ControlFunctionRegistry final {
 public:
  static constexpr std::size_t kCapacity = 64U;

  void reset() noexcept;

  [[nodiscard]] RegistryStatus observe(
      const transport::CanFrame& frame) noexcept;

  [[nodiscard]] LookupStatus find_by_name(
      std::uint64_t name,
      ControlFunctionRecord& record) const noexcept;

  [[nodiscard]] LookupStatus find_by_address(
      std::uint8_t source_address,
      ControlFunctionRecord& record) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] ControlFunctionRegistryCounters counters()
      const noexcept;

 private:
  [[nodiscard]] std::size_t find_name_slot(
      std::uint64_t name) const noexcept;
  [[nodiscard]] std::size_t find_free_slot() const noexcept;
  [[nodiscard]] bool address_is_ambiguous(
      std::uint8_t source_address,
      std::uint64_t except_name) const noexcept;

  std::array<ControlFunctionRecord, kCapacity> records_{};
  std::size_t size_{0U};
  ControlFunctionRegistryCounters counters_{};
};

enum class NetworkManagementStatus : std::uint8_t {
  ok,
  no_action,
  invalid_state,
  queue_overflow,
  protocol_fault,
  registry_capacity_exhausted,
};

struct NetworkManagementCounters {
  j1939::NetworkManagerCounters local{};
  ControlFunctionRegistryCounters registry{};
};

class NetworkManagement final : public transport::ICanFrameSink {
 public:
  [[nodiscard]] bool configure(
      const j1939::AddressClaimConfig& config) noexcept;

  [[nodiscard]] NetworkManagementStatus start(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] NetworkManagementStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] NetworkManagementStatus on_transport_message(
      const j1939::TpMessage& message,
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;

  [[nodiscard]] LookupStatus find_by_name(
      std::uint64_t name,
      ControlFunctionRecord& record) const noexcept;

  [[nodiscard]] LookupStatus find_by_address(
      std::uint8_t source_address,
      ControlFunctionRecord& record) const noexcept;

  [[nodiscard]] j1939::AddressClaimState
  local_address_claim_state() const noexcept;

  [[nodiscard]] std::uint8_t local_address() const noexcept;
  [[nodiscard]] NetworkManagementStatus status() const noexcept;
  [[nodiscard]] NetworkManagementCounters counters() const noexcept;
  [[nodiscard]] std::size_t registered_control_functions()
      const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;

  [[nodiscard]] static transport::CanFilter rx_filter() noexcept;

 private:
  [[nodiscard]] static NetworkManagementStatus translate(
      j1939::NetworkManagerStatus status) noexcept;
  void update_local_status() noexcept;

  j1939::NetworkManager local_{};
  ControlFunctionRegistry registry_{};
  bool configured_{false};
  bool registry_capacity_exhausted_{false};
  NetworkManagementStatus status_{
      NetworkManagementStatus::invalid_state};
};

}  // namespace ecu::core::v2::protocol::isobus
