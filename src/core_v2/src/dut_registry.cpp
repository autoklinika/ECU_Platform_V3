#include "ecu/core_v2/runtime/dut_registry.hpp"

#include <limits>

namespace ecu::core::v2::runtime {

DutRegistrationResult DutRegistry::register_dut(
    const domain::DutDescriptor& descriptor) noexcept {
  DutRegistrationResult result{};
  if (!configuration_.accepts_registration()) {
    result.status = DutRegistrationStatus::configuration_frozen;
    return result;
  }
  if (!domain::is_valid_dut_descriptor(descriptor)) {
    return result;
  }

  for (std::size_t index = 0U;
       index < entries_.size();
       ++index) {
    const auto& entry = entries_[index];
    if (!entry.used) {
      continue;
    }

    if (entry.descriptor.profile_id == descriptor.profile_id) {
      if (entry.descriptor.dut_class == descriptor.dut_class &&
          entry.descriptor.domains == descriptor.domains &&
          entry.descriptor.capabilities == descriptor.capabilities) {
        result.status = DutRegistrationStatus::already_registered;
        result.handle = {
            static_cast<std::uint16_t>(index),
            entry.generation};
      } else {
        result.status =
            DutRegistrationStatus::profile_id_conflict;
      }
      return result;
    }
  }

  for (std::size_t index = 0U;
       index < entries_.size();
       ++index) {
    auto& entry = entries_[index];
    if (entry.used) {
      continue;
    }

    const auto generation = next_generation();
    if (generation == 0U) {
      result.status = DutRegistrationStatus::capacity_exhausted;
      return result;
    }

    entry.used = true;
    entry.generation = generation;
    entry.descriptor = descriptor;
    result.status = DutRegistrationStatus::registered;
    result.handle = {
        static_cast<std::uint16_t>(index),
        generation};
    return result;
  }

  result.status = DutRegistrationStatus::capacity_exhausted;
  return result;
}

bool DutRegistry::freeze_configuration() noexcept {
  if (dut_count() == 0U) {
    return false;
  }
  return configuration_.freeze();
}

const domain::DutDescriptor* DutRegistry::get(
    const DutHandle handle) const noexcept {
  if (configuration_.state() != ConfigurationState::frozen ||
      !handle.valid() ||
      static_cast<std::size_t>(handle.slot) >= entries_.size()) {
    return nullptr;
  }

  const auto& entry = entries_[handle.slot];
  return entry.used &&
                 entry.generation == handle.generation
             ? &entry.descriptor
             : nullptr;
}

const domain::DutDescriptor* DutRegistry::find_by_profile_id(
    const domain::DutProfileId profile_id) const noexcept {
  if (configuration_.state() != ConfigurationState::frozen ||
      profile_id == 0U) {
    return nullptr;
  }

  for (const auto& entry : entries_) {
    if (entry.used &&
        entry.descriptor.profile_id == profile_id) {
      return &entry.descriptor;
    }
  }
  return nullptr;
}

std::size_t DutRegistry::dut_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& entry : entries_) {
    if (entry.used) {
      ++count;
    }
  }
  return count;
}

ConfigurationState DutRegistry::configuration_state()
    const noexcept {
  return configuration_.state();
}

std::uint16_t DutRegistry::next_generation() noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint16_t>::max)();
  if (generation_counter_ == maximum) {
    return 0U;
  }

  ++generation_counter_;
  return generation_counter_;
}

}  // namespace ecu::core::v2::runtime
