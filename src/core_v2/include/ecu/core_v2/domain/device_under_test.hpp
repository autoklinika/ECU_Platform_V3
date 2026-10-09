#pragma once

#include "ecu/core_v2/domain/product_scope.hpp"

#include <cstdint>

namespace ecu::core::v2::domain {

enum class DutClass : std::uint8_t {
  ecu = 0U,
  actuator = 1U,
  sensor = 2U,
  gateway = 3U,
  network_node = 4U,
  other = 5U,
};

enum class DutCapability : std::uint8_t {
  raw_can = 0U,
  cyclic_can = 1U,
  can_fd = 2U,
  j1939 = 3U,
  isotp = 4U,
  uds = 5U,
  isobus = 6U,
  doip = 7U,
  active_control = 8U,
  feedback = 9U,
  requires_environment = 10U,
  requires_power_control = 11U,
  requires_wake = 12U,
};

using DutCapabilityMask = std::uint32_t;
using DutProfileId = std::uint32_t;

[[nodiscard]] constexpr DutCapabilityMask dut_capability_mask(
    const DutCapability capability) noexcept {
  const auto bit = static_cast<std::uint8_t>(capability);
  return bit < 32U
             ? static_cast<DutCapabilityMask>(
                   static_cast<DutCapabilityMask>(1U) << bit)
             : 0U;
}

[[nodiscard]] constexpr bool has_dut_capability(
    const DutCapabilityMask mask,
    const DutCapability capability) noexcept {
  const auto required = dut_capability_mask(capability);
  return required != 0U && (mask & required) == required;
}

struct DutDescriptor {
  DutProfileId profile_id{0U};
  DutClass dut_class{DutClass::other};
  DomainMask domains{0U};
  DutCapabilityMask capabilities{0U};
};

[[nodiscard]] constexpr bool is_valid_dut_descriptor(
    const DutDescriptor& descriptor) noexcept {
  if (descriptor.profile_id == 0U ||
      !is_supported_domain_mask(descriptor.domains)) {
    return false;
  }

  if (has_dut_capability(
          descriptor.capabilities,
          DutCapability::cyclic_can) &&
      !has_dut_capability(
          descriptor.capabilities,
          DutCapability::raw_can)) {
    return false;
  }

  if (has_dut_capability(
          descriptor.capabilities,
          DutCapability::uds) &&
      !has_dut_capability(
          descriptor.capabilities,
          DutCapability::isotp) &&
      !has_dut_capability(
          descriptor.capabilities,
          DutCapability::doip)) {
    return false;
  }

  if (has_dut_capability(
          descriptor.capabilities,
          DutCapability::active_control) &&
      descriptor.dut_class == DutClass::sensor) {
    return false;
  }

  return true;
}

}  // namespace ecu::core::v2::domain
