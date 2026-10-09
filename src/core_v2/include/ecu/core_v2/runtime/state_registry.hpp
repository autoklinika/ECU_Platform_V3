#pragma once

#include "ecu/core_v2/runtime/configuration_gate.hpp"
#include "ecu/core_v2/runtime/state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

enum class StateSnapshotStatus : std::uint8_t {
  ok,
  invalid_argument,
  configuration_not_frozen,
  buffer_too_small,
  unavailable,
};

class IStateProvider {
 public:
  [[nodiscard]] virtual StateTypeId state_type() const noexcept = 0;
  [[nodiscard]] virtual StateRevision revision() const noexcept = 0;

  [[nodiscard]] virtual StateSnapshotStatus snapshot(
      std::byte* destination,
      std::size_t capacity,
      std::size_t& length,
      StateHeader& header) const noexcept = 0;

 protected:
  ~IStateProvider() = default;
};

struct StateProviderExecutionContract {
  time::MonotonicDuration max_snapshot_duration{0};
};

enum class StateRegistrationStatus : std::uint8_t {
  registered,
  already_registered,
  type_conflict,
  invalid_argument,
  configuration_frozen,
  capacity_exhausted,
};

class StateRegistry final {
 public:
  static constexpr std::size_t kMaxProviders = 64U;

  [[nodiscard]] StateRegistrationStatus register_provider(
      IStateProvider& provider,
      StateProviderExecutionContract execution) noexcept;

  [[nodiscard]] bool freeze_configuration() noexcept;

  [[nodiscard]] StateSnapshotStatus snapshot(
      StateTypeId type,
      std::byte* destination,
      std::size_t capacity,
      std::size_t& length,
      StateHeader& header) const noexcept;

  [[nodiscard]] const IStateProvider* find(
      StateTypeId type) const noexcept;

  [[nodiscard]] std::size_t provider_count() const noexcept;
  [[nodiscard]] ConfigurationState configuration_state()
      const noexcept;

  [[nodiscard]] time::MonotonicDuration
  max_snapshot_duration(StateTypeId type) const noexcept;

 private:
  struct Entry {
    StateTypeId type{0U};
    IStateProvider* provider{nullptr};
    StateProviderExecutionContract execution{};
  };

  [[nodiscard]] Entry* find_entry(StateTypeId type) noexcept;
  [[nodiscard]] const Entry* find_entry(
      StateTypeId type) const noexcept;
  [[nodiscard]] Entry* find_free() noexcept;

  ConfigurationGate configuration_{};
  std::array<Entry, kMaxProviders> entries_{};
};

}  // namespace ecu::core::v2::runtime
