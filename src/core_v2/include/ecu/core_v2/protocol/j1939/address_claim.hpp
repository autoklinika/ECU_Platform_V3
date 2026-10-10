#pragma once

#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"
#include "ecu/core_v2/protocol/j1939/j1939_name.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

enum class AddressClaimState : std::uint8_t {
  idle,
  claiming,
  claimed,
  cannot_claim,
  faulted,
};

enum class AddressClaimStatus : std::uint8_t {
  ok,
  no_action,
  invalid_argument,
  invalid_state,
  invalid_time,
  duplicate_name,
  faulted,
};

enum class AddressClaimTxKind : std::uint8_t {
  none,
  address_claim,
  cannot_claim,
};

struct AddressClaimedMessage {
  std::uint8_t source_address{kNullAddress};
  std::uint64_t raw_name{0U};
};

[[nodiscard]] bool decode_address_claimed(
    const transport::CanFrame& frame,
    AddressClaimedMessage& message) noexcept;

struct AddressClaimConfig {
  static constexpr std::size_t kMaxAlternativeAddresses = 16U;

  NameFields name{};
  std::uint8_t preferred_address{0U};
  std::array<std::uint8_t, kMaxAlternativeAddresses>
      alternative_addresses{};
  std::uint8_t alternative_count{0U};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
  time::MonotonicDuration cannot_claim_response_delay{0};
  bool accept_commanded_address{false};
};

struct AddressClaimStep {
  AddressClaimStatus status{AddressClaimStatus::no_action};
  AddressClaimTxKind tx_kind{AddressClaimTxKind::none};
  transport::CanFrame tx_frame{};
  bool address_changed{false};
  bool became_claimed{false};
  bool address_lost{false};
  bool conflict_observed{false};
};

class AddressClaimEngine {
 public:
  inline static constexpr time::MonotonicDuration
      kClaimStabilizationDelay{250000000};
  inline static constexpr time::MonotonicDuration
      kMaxCannotClaimResponseDelay{153000000};

  [[nodiscard]] bool configure(
      const AddressClaimConfig& config) noexcept;

  [[nodiscard]] AddressClaimStep begin(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] AddressClaimStep on_frame(
      const transport::ReceivedCanFrame& frame) noexcept;

  [[nodiscard]] AddressClaimStep poll(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] AddressClaimStep command_address(
      std::uint64_t target_name,
      std::uint8_t commanded_address,
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] AddressClaimState state() const noexcept;
  [[nodiscard]] std::uint8_t current_address() const noexcept;
  [[nodiscard]] std::uint64_t raw_name() const noexcept;
  [[nodiscard]] bool configured() const noexcept;

 private:
  [[nodiscard]] bool valid_config(
      const AddressClaimConfig& config,
      std::uint64_t& raw_name) const noexcept;
  [[nodiscard]] bool observe_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;
  [[nodiscard]] bool upper_bound(
      const time::MonotonicClockReading& reading,
      time::MonotonicTime& value) const noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) noexcept;
  [[nodiscard]] bool requires_stabilization_delay(
      std::uint8_t address) const noexcept;
  [[nodiscard]] bool next_address(
      std::uint8_t& address) noexcept;
  [[nodiscard]] AddressClaimStep start_claim(
      std::uint8_t address,
      const time::MonotonicClockReading& now,
      bool address_changed) noexcept;
  [[nodiscard]] AddressClaimStep lose_address(
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] bool build_claim_frame(
      std::uint8_t source_address,
      transport::CanFrame& frame) const noexcept;
  [[nodiscard]] bool addressed_to_us_or_global(
      const IdentifierFields& fields) const noexcept;
  [[nodiscard]] AddressClaimStep schedule_cannot_claim_response(
      const time::MonotonicClockReading& now) noexcept;

  AddressClaimConfig config_{};
  std::uint64_t raw_name_{0U};
  std::uint8_t current_address_{kNullAddress};
  std::uint8_t next_alternative_{0U};
  AddressClaimState state_{AddressClaimState::idle};
  time::MonotonicTime claim_deadline_{0};
  time::MonotonicTime cannot_claim_response_deadline_{0};
  time::MonotonicTime last_observed_time_{0};
  bool configured_{false};
  bool has_last_observed_time_{false};
  bool pending_cannot_claim_response_{false};
};

}  // namespace ecu::core::v2::protocol::j1939
