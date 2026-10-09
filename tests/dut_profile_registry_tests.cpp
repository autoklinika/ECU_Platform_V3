#include "ecu/dut_profile/registry.hpp"

#include <cstdint>
#include <iostream>

namespace domain = ecu::core::v2::domain;
namespace dp = ecu::dut_profile;
namespace runtime = ecu::core::v2::runtime;
namespace transport = ecu::core::v2::transport;

namespace {

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

[[nodiscard]] dp::DutProfileDefinition make_profile(
    const domain::DutProfileId id,
    const std::uint32_t revision) {
  dp::DutProfileDefinition profile{};
  profile.profile_revision = revision;
  profile.dut = {
      id,
      domain::DutClass::ecu,
      domain::kTruck | domain::kAgri | domain::kOhv,
      domain::dut_capability_mask(domain::DutCapability::raw_can)};
  profile.resources[0U] = {
      10U,
      runtime::ResourceClass::can_channel};
  profile.resource_count = 1U;
  profile.can_links[0U] = {
      1U,
      10U,
      500000U,
      false,
      0U,
      transport::CanMode::normal};
  profile.can_link_count = 1U;
  profile.rx_expectations[0U] = {
      1U,
      0x123U,
      0x7FFU,
      true,
      false};
  profile.rx_expectation_count = 1U;
  return profile;
}

}  // namespace

int main() {
  int failures = 0;

  {
    dp::DutProfileRegistry registry;

    failures += require(
        registry.profile_count() == 0U &&
            registry.configuration_state() ==
                runtime::ConfigurationState::configuring,
        "registry starts empty and configurable");

    failures += require(
        registry.select(100U).status ==
            dp::ProfileSelectionStatus::registry_not_frozen,
        "selection is forbidden before topology freeze");

    auto invalid = make_profile(100U, 1U);
    invalid.profile_revision = 0U;
    failures += require(
        registry.register_profile(invalid) ==
            dp::ProfileRegistrationStatus::invalid_profile,
        "invalid profile is rejected before storage");

    auto first = make_profile(100U, 1U);
    auto second = make_profile(200U, 4U);

    failures += require(
        registry.register_profile(first) ==
                dp::ProfileRegistrationStatus::registered &&
            registry.register_profile(second) ==
                dp::ProfileRegistrationStatus::registered &&
            registry.profile_count() == 2U,
        "validated profiles register in deterministic order");

    const auto* stored_first = registry.at(0U);
    const auto* stored_second = registry.at(1U);
    failures += require(
        stored_first != nullptr &&
            stored_second != nullptr &&
            stored_first->dut.profile_id == 100U &&
            stored_second->dut.profile_id == 200U &&
            registry.at(2U) == nullptr,
        "registry exposes bounded index enumeration");

    first.profile_revision = 99U;
    first.can_links[0U].nominal_bitrate = 125000U;
    failures += require(
        stored_first != nullptr &&
            stored_first->profile_revision == 1U &&
            stored_first->can_links[0U].nominal_bitrate == 500000U,
        "registry owns immutable copies of registered definitions");

    auto conflicting = make_profile(100U, 8U);
    failures += require(
        registry.register_profile(conflicting) ==
            dp::ProfileRegistrationStatus::duplicate_profile_id,
        "profile id uniquely identifies one registered definition");

    failures += require(
        registry.freeze_configuration() &&
            registry.configuration_state() ==
                runtime::ConfigurationState::frozen &&
            !registry.freeze_configuration(),
        "profile topology freezes exactly once");

    const auto selected = registry.select(200U);
    failures += require(
        selected.status == dp::ProfileSelectionStatus::selected &&
            selected.profile != nullptr &&
            selected.profile->dut.profile_id == 200U &&
            selected.profile->profile_revision == 4U,
        "frozen registry selects profile by stable profile id");

    failures += require(
        registry.select(0U).status ==
                dp::ProfileSelectionStatus::invalid_profile_id &&
            registry.select(999U).status ==
                dp::ProfileSelectionStatus::not_found,
        "selection distinguishes invalid id from unknown id");

    failures += require(
        registry.register_profile(make_profile(300U, 1U)) ==
            dp::ProfileRegistrationStatus::configuration_frozen,
        "frozen profile topology rejects later registration");
  }

  {
    dp::DutProfileRegistry registry;
    for (std::size_t index = 0U;
         index < dp::DutProfileRegistry::kMaxProfiles;
         ++index) {
      const auto status = registry.register_profile(
          make_profile(
              static_cast<domain::DutProfileId>(1000U + index),
              1U));
      failures += require(
          status == dp::ProfileRegistrationStatus::registered,
          "fixed registry accepts entries up to declared capacity");
    }

    failures += require(
        registry.profile_count() == dp::DutProfileRegistry::kMaxProfiles,
        "registry count reaches exact fixed capacity");

    failures += require(
        registry.register_profile(make_profile(5000U, 1U)) ==
            dp::ProfileRegistrationStatus::capacity_exhausted,
        "registry fails closed when fixed capacity is exhausted");
  }

  if (failures == 0) {
    std::cout << "DUT_PROFILE_REGISTRY_V1=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
