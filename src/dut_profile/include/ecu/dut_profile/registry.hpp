#pragma once

#include "ecu/dut_profile/profile.hpp"
#include "ecu/core_v2/runtime/configuration_gate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::dut_profile {

enum class ProfileRegistrationStatus : std::uint8_t {
  registered,
  invalid_profile,
  duplicate_profile_id,
  configuration_frozen,
  capacity_exhausted,
};

enum class ProfileSelectionStatus : std::uint8_t {
  selected,
  invalid_profile_id,
  registry_not_frozen,
  not_found,
};

struct ProfileSelectionResult {
  ProfileSelectionStatus status{ProfileSelectionStatus::invalid_profile_id};
  const DutProfileDefinition* profile{nullptr};
};

class DutProfileRegistry final {
 public:
  static constexpr std::size_t kMaxProfiles = 32U;

  [[nodiscard]] ProfileRegistrationStatus register_profile(
      const DutProfileDefinition& definition) noexcept;

  [[nodiscard]] bool freeze_configuration() noexcept;

  [[nodiscard]] ProfileSelectionResult select(
      ecu::core::v2::domain::DutProfileId profile_id) const noexcept;

  [[nodiscard]] const DutProfileDefinition* at(
      std::size_t index) const noexcept;

  [[nodiscard]] std::size_t profile_count() const noexcept;

  [[nodiscard]] ecu::core::v2::runtime::ConfigurationState
  configuration_state() const noexcept;

 private:
  [[nodiscard]] const DutProfileDefinition* find(
      ecu::core::v2::domain::DutProfileId profile_id) const noexcept;

  ecu::core::v2::runtime::ConfigurationGate configuration_{};
  std::array<DutProfileDefinition, kMaxProfiles> profiles_{};
  std::size_t profile_count_{0U};
};

}  // namespace ecu::dut_profile
