#include "ecu/core_v2/runtime/state_registry.hpp"

namespace ecu::core::v2::runtime {

StateRegistrationStatus StateRegistry::register_provider(
    IStateProvider& provider,
    const StateProviderExecutionContract execution) noexcept {
  if (!configuration_.accepts_registration()) {
    return StateRegistrationStatus::configuration_frozen;
  }

  const auto type = provider.state_type();
  if (type == 0U ||
      execution.max_snapshot_duration.count() <= 0) {
    return StateRegistrationStatus::invalid_argument;
  }

  if (auto* existing = find_entry(type);
      existing != nullptr) {
    return existing->provider == &provider
               ? StateRegistrationStatus::already_registered
               : StateRegistrationStatus::type_conflict;
  }

  auto* slot = find_free();
  if (slot == nullptr) {
    return StateRegistrationStatus::capacity_exhausted;
  }

  slot->type = type;
  slot->provider = &provider;
  slot->execution = execution;
  return StateRegistrationStatus::registered;
}

bool StateRegistry::freeze_configuration() noexcept {
  if (provider_count() == 0U) {
    return false;
  }
  return configuration_.freeze();
}

StateSnapshotStatus StateRegistry::snapshot(
    const StateTypeId type,
    std::byte* destination,
    const std::size_t capacity,
    std::size_t& length,
    StateHeader& header) const noexcept {
  length = 0U;
  header = {};

  if (type == 0U ||
      (capacity > 0U && destination == nullptr)) {
    return StateSnapshotStatus::invalid_argument;
  }
  if (configuration_.state() != ConfigurationState::frozen) {
    return StateSnapshotStatus::configuration_not_frozen;
  }

  const auto* entry = find_entry(type);
  if (entry == nullptr || entry->provider == nullptr) {
    return StateSnapshotStatus::unavailable;
  }

  return entry->provider->snapshot(
      destination,
      capacity,
      length,
      header);
}

const IStateProvider* StateRegistry::find(
    const StateTypeId type) const noexcept {
  if (configuration_.state() != ConfigurationState::frozen) {
    return nullptr;
  }

  const auto* entry = find_entry(type);
  return entry != nullptr ? entry->provider : nullptr;
}

std::size_t StateRegistry::provider_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& entry : entries_) {
    if (entry.provider != nullptr) {
      ++count;
    }
  }
  return count;
}

ConfigurationState StateRegistry::configuration_state()
    const noexcept {
  return configuration_.state();
}

time::MonotonicDuration StateRegistry::max_snapshot_duration(
    const StateTypeId type) const noexcept {
  const auto* entry = find_entry(type);
  return entry != nullptr
             ? entry->execution.max_snapshot_duration
             : time::MonotonicDuration{0};
}

StateRegistry::Entry* StateRegistry::find_entry(
    const StateTypeId type) noexcept {
  for (auto& entry : entries_) {
    if (entry.provider != nullptr && entry.type == type) {
      return &entry;
    }
  }
  return nullptr;
}

const StateRegistry::Entry* StateRegistry::find_entry(
    const StateTypeId type) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.provider != nullptr && entry.type == type) {
      return &entry;
    }
  }
  return nullptr;
}

StateRegistry::Entry* StateRegistry::find_free() noexcept {
  for (auto& entry : entries_) {
    if (entry.provider == nullptr) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace ecu::core::v2::runtime
