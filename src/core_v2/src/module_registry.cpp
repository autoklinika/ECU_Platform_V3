#include "ecu/core_v2/runtime/module_registry.hpp"

namespace ecu::core::v2::runtime {

ModuleRegistrationStatus ModuleRegistry::register_module(
    ICoreModule& module) noexcept {
  if (!configuration_.accepts_registration()) {
    return ModuleRegistrationStatus::configuration_frozen;
  }

  const auto descriptor = module.descriptor();
  if (!is_valid_module_descriptor(descriptor)) {
    return ModuleRegistrationStatus::invalid_argument;
  }

  for (auto* existing : modules_) {
    if (existing == &module) {
      return ModuleRegistrationStatus::already_registered;
    }
    if (existing != nullptr &&
        existing->descriptor().id == descriptor.id) {
      return ModuleRegistrationStatus::id_conflict;
    }
  }

  for (auto& slot : modules_) {
    if (slot == nullptr) {
      slot = &module;
      return ModuleRegistrationStatus::registered;
    }
  }

  return ModuleRegistrationStatus::capacity_exhausted;
}

bool ModuleRegistry::freeze_configuration() noexcept {
  if (module_count() == 0U) {
    return false;
  }
  return configuration_.freeze();
}

ICoreModule* ModuleRegistry::find(
    const ModuleId id) noexcept {
  if (configuration_.state() != ConfigurationState::frozen ||
      id == 0U) {
    return nullptr;
  }

  for (auto* module : modules_) {
    if (module != nullptr &&
        module->descriptor().id == id) {
      return module;
    }
  }
  return nullptr;
}

const ICoreModule* ModuleRegistry::find(
    const ModuleId id) const noexcept {
  if (configuration_.state() != ConfigurationState::frozen ||
      id == 0U) {
    return nullptr;
  }

  for (const auto* module : modules_) {
    if (module != nullptr &&
        module->descriptor().id == id) {
      return module;
    }
  }
  return nullptr;
}

std::size_t ModuleRegistry::module_count() const noexcept {
  std::size_t count = 0U;
  for (const auto* module : modules_) {
    if (module != nullptr) {
      ++count;
    }
  }
  return count;
}

ConfigurationState ModuleRegistry::configuration_state()
    const noexcept {
  return configuration_.state();
}

}  // namespace ecu::core::v2::runtime
