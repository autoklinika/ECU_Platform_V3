#pragma once

#include <cstdint>

namespace ecu::core::v2::domain {

enum class MachineDomain : std::uint8_t {
  truck = 0U,
  agri = 1U,
  ohv = 2U,
};

using DomainMask = std::uint8_t;

[[nodiscard]] constexpr DomainMask domain_mask(
    const MachineDomain domain) noexcept {
  switch (domain) {
    case MachineDomain::truck:
      return static_cast<DomainMask>(1U << 0U);
    case MachineDomain::agri:
      return static_cast<DomainMask>(1U << 1U);
    case MachineDomain::ohv:
      return static_cast<DomainMask>(1U << 2U);
  }
  return 0U;
}

inline constexpr DomainMask kTruck =
    domain_mask(MachineDomain::truck);
inline constexpr DomainMask kAgri =
    domain_mask(MachineDomain::agri);
inline constexpr DomainMask kOhv =
    domain_mask(MachineDomain::ohv);
inline constexpr DomainMask kAllSupported =
    static_cast<DomainMask>(kTruck | kAgri | kOhv);

[[nodiscard]] constexpr bool is_supported_domain_mask(
    const DomainMask mask) noexcept {
  return mask != 0U &&
         (mask & static_cast<DomainMask>(~kAllSupported)) == 0U;
}

}  // namespace ecu::core::v2::domain
