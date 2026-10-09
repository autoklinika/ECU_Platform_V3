#pragma once

#include "ecu/core_v2/domain/device_under_test.hpp"
#include "ecu/core_v2/runtime/configuration_gate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

struct DutHandle {
  std::uint16_t slot{0U};
  std::uint16_t generation{0U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return generation != 0U;
  }
};

enum class DutRegistrationStatus : std::uint8_t {
  registered,
  already_registered,
  profile_id_conflict,
  invalid_argument,
  configuration_frozen,
  capacity_exhausted,
};

struct DutRegistrationResult {
  DutRegistrationStatus status{
      DutRegistrationStatus::invalid_argument};
  DutHandle handle{};
};

class DutRegistry final {
 public:
  static constexpr std::size_t kMaxDuts = 64U;

  [[nodiscard]] DutRegistrationResult register_dut(
      const domain::DutDescriptor& descriptor) noexcept;

  [[nodiscard]] bool freeze_configuration() noexcept;

  [[nodiscard]] const domain::DutDescriptor* get(
      DutHandle handle) const noexcept;

  [[nodiscard]] const domain::DutDescriptor* find_by_profile_id(
      domain::DutProfileId profile_id) const noexcept;

  [[nodiscard]] std::size_t dut_count() const noexcept;
  [[nodiscard]] ConfigurationState configuration_state()
      const noexcept;

 private:
  struct Entry {
    bool used{false};
    std::uint16_t generation{0U};
    domain::DutDescriptor descriptor{};
  };

  [[nodiscard]] std::uint16_t next_generation() noexcept;

  ConfigurationGate configuration_{};
  std::array<Entry, kMaxDuts> entries_{};
  std::uint16_t generation_counter_{0U};
};

}  // namespace ecu::core::v2::runtime
