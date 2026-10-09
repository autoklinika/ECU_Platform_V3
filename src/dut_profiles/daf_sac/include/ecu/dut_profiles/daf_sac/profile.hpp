#pragma once

#include "ecu/dut_profile/profile.hpp"

#include <cstdint>

namespace ecu::dut_profiles::daf_sac {

enum class CanBitrateProfile : std::uint8_t {
  k250k = 0U,
  k500k = 1U,
};

inline constexpr std::uint32_t kProfileRevision = 2U; // adds pressure RX declaration
inline constexpr ecu::dut_profile::ResourceRoleId kPrimaryCanRole = 10U;
inline constexpr ecu::dut_profile::CanLinkId kPrimaryCanLink = 1U;

inline constexpr std::uint32_t kRequestCanId = 0x18DA30F9U;
inline constexpr std::uint32_t kResponseCanId = 0x18DAF930U;
inline constexpr std::uint8_t kEcuSourceAddress = 0x30U;
inline constexpr std::uint8_t kTesterSourceAddress = 0xF9U;

inline constexpr std::uint16_t kDidVin = 0xF190U;
inline constexpr std::uint16_t kDidSoftware = 0xF188U;
inline constexpr std::uint16_t kDidHardware = 0xF192U;
inline constexpr std::uint16_t kDidVoltage = 0xFE96U;
inline constexpr std::uint32_t kPressurePgn = 65198U;
inline constexpr std::uint32_t kPressureCanId = 0x18FEAE30U;
inline constexpr std::uint32_t kPressureCanMask = 0x03FFFFFFU;

[[nodiscard]] constexpr ecu::core::v2::domain::DutProfileId profile_id(
    const CanBitrateProfile bitrate) noexcept {
  return bitrate == CanBitrateProfile::k250k
             ? 0xDAF00025U
             : 0xDAF00050U;
}

[[nodiscard]] constexpr std::uint32_t nominal_bitrate(
    const CanBitrateProfile bitrate) noexcept {
  return bitrate == CanBitrateProfile::k250k
             ? 250000U
             : 500000U;
}

[[nodiscard]] ecu::dut_profile::DutProfileDefinition
make_profile_definition(CanBitrateProfile bitrate) noexcept;

}  // namespace ecu::dut_profiles::daf_sac
