#include "ecu/dut_profile/registry.hpp"

namespace ecu::dut_profile {

ProfileRegistrationStatus DutProfileRegistry::register_profile(
    const DutProfileDefinition& definition) noexcept {
  if (configuration_.state() ==
      ecu::core::v2::runtime::ConfigurationState::frozen) {
    return ProfileRegistrationStatus::configuration_frozen;
  }

  if (validate_profile_definition(definition) !=
      ProfileValidationStatus::valid) {
    return ProfileRegistrationStatus::invalid_profile;
  }

  if (find(definition.dut.profile_id) != nullptr) {
    return ProfileRegistrationStatus::duplicate_profile_id;
  }

  if (profile_count_ >= profiles_.size()) {
    return ProfileRegistrationStatus::capacity_exhausted;
  }

  profiles_[profile_count_] = definition;
  ++profile_count_;
  return ProfileRegistrationStatus::registered;
}

bool DutProfileRegistry::freeze_configuration() noexcept {
  return configuration_.freeze();
}

ProfileSelectionResult DutProfileRegistry::select(
    const ecu::core::v2::domain::DutProfileId profile_id) const noexcept {
  if (profile_id == 0U) {
    return {ProfileSelectionStatus::invalid_profile_id, nullptr};
  }

  if (configuration_.state() !=
      ecu::core::v2::runtime::ConfigurationState::frozen) {
    return {ProfileSelectionStatus::registry_not_frozen, nullptr};
  }

  const auto* profile = find(profile_id);
  if (profile == nullptr) {
    return {ProfileSelectionStatus::not_found, nullptr};
  }

  return {ProfileSelectionStatus::selected, profile};
}

const DutProfileDefinition* DutProfileRegistry::at(
    const std::size_t index) const noexcept {
  return index < profile_count_ ? &profiles_[index] : nullptr;
}

std::size_t DutProfileRegistry::profile_count() const noexcept {
  return profile_count_;
}

ecu::core::v2::runtime::ConfigurationState
DutProfileRegistry::configuration_state() const noexcept {
  return configuration_.state();
}

const DutProfileDefinition* DutProfileRegistry::find(
    const ecu::core::v2::domain::DutProfileId profile_id) const noexcept {
  for (std::size_t index = 0U; index < profile_count_; ++index) {
    if (profiles_[index].dut.profile_id == profile_id) {
      return &profiles_[index];
    }
  }
  return nullptr;
}

}  // namespace ecu::dut_profile
