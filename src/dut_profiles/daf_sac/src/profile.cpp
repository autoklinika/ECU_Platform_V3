#include "ecu/dut_profiles/daf_sac/profile.hpp"

namespace ecu::dut_profiles::daf_sac {
namespace {

[[nodiscard]] constexpr ecu::core::v2::domain::DutCapabilityMask cap(
    const ecu::core::v2::domain::DutCapability value) noexcept {
  return ecu::core::v2::domain::dut_capability_mask(value);
}

}  // namespace

ecu::dut_profile::DutProfileDefinition
make_profile_definition(const CanBitrateProfile bitrate) noexcept {
  namespace domain = ecu::core::v2::domain;
  namespace dp = ecu::dut_profile;
  namespace runtime = ecu::core::v2::runtime;
  namespace transport = ecu::core::v2::transport;

  dp::DutProfileDefinition profile{};
  profile.profile_revision = kProfileRevision;
  profile.dut = {
      profile_id(bitrate),
      domain::DutClass::ecu,
      domain::kTruck,
      static_cast<domain::DutCapabilityMask>(
          cap(domain::DutCapability::raw_can) |
          cap(domain::DutCapability::j1939) |
          cap(domain::DutCapability::isotp) |
          cap(domain::DutCapability::uds))};

  profile.required_protocols =
      dp::protocol_requirement_mask(dp::ProtocolRequirement::isotp) |
      dp::protocol_requirement_mask(dp::ProtocolRequirement::uds);

  profile.resources[0U] = {
      kPrimaryCanRole,
      runtime::ResourceClass::can_channel};
  profile.resource_count = 1U;

  profile.can_links[0U] = {
      kPrimaryCanLink,
      kPrimaryCanRole,
      nominal_bitrate(bitrate),
      false,
      0U,
      transport::CanMode::normal};
  profile.can_link_count = 1U;

  profile.rx_expectations[0U] = {
      kPrimaryCanLink,
      kResponseCanId,
      0x1FFFFFFFU,
      false,
      true};
  profile.rx_expectations[1U] = {
      kPrimaryCanLink,
      kPressureCanId,
      kPressureCanMask,
      false,
      true};
  profile.rx_expectation_count = 2U;

  return profile;
}

}  // namespace ecu::dut_profiles::daf_sac
