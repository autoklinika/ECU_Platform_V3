#include "ecu/core_v2/runtime/resource_manager.hpp"

#include <limits>

namespace ecu::core::v2::runtime {

ResourceAcquireResult ResourceManager::acquire(
    const ResourceKey resource,
    const ResourceOwnerId owner) noexcept {
  ResourceAcquireResult result{};
  if (owner == 0U) {
    return result;
  }

  if (auto* existing = find_entry(resource);
      existing != nullptr) {
    result.lease = {
        existing->resource,
        existing->owner,
        existing->generation};
    result.status =
        existing->owner == owner
            ? ResourceAcquireStatus::already_owned
            : ResourceAcquireStatus::busy;
    return result;
  }

  auto* entry = find_free_entry();
  if (entry == nullptr) {
    result.status = ResourceAcquireStatus::capacity_exhausted;
    return result;
  }

  const auto generation = next_generation();
  if (generation == 0U) {
    result.status =
        ResourceAcquireStatus::generation_exhausted;
    return result;
  }

  entry->used = true;
  entry->resource = resource;
  entry->owner = owner;
  entry->generation = generation;

  result.status = ResourceAcquireStatus::acquired;
  result.lease = {resource, owner, generation};
  return result;
}

ResourceReleaseStatus ResourceManager::release(
    const ResourceLease& lease) noexcept {
  if (!lease.valid()) {
    return ResourceReleaseStatus::invalid_lease;
  }

  auto* entry = find_entry(lease.resource);
  if (entry == nullptr) {
    return ResourceReleaseStatus::stale_lease;
  }
  if (entry->owner != lease.owner) {
    return ResourceReleaseStatus::not_owner;
  }
  if (entry->generation != lease.generation) {
    return ResourceReleaseStatus::stale_lease;
  }

  *entry = {};
  return ResourceReleaseStatus::released;
}

bool ResourceManager::owns(
    const ResourceKey resource,
    const ResourceOwnerId owner) const noexcept {
  if (owner == 0U) {
    return false;
  }

  const auto* entry = find_entry(resource);
  return entry != nullptr && entry->owner == owner;
}

bool ResourceManager::owns(
    const ResourceLease& lease) const noexcept {
  if (!lease.valid()) {
    return false;
  }

  const auto* entry = find_entry(lease.resource);
  return entry != nullptr &&
         entry->owner == lease.owner &&
         entry->generation == lease.generation;
}

ResourceOwnerId ResourceManager::current_owner(
    const ResourceKey resource) const noexcept {
  const auto* entry = find_entry(resource);
  return entry != nullptr ? entry->owner : 0U;
}

std::size_t ResourceManager::active_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& entry : entries_) {
    if (entry.used) {
      ++count;
    }
  }
  return count;
}

ResourceManager::Entry* ResourceManager::find_entry(
    const ResourceKey resource) noexcept {
  for (auto& entry : entries_) {
    if (entry.used && entry.resource == resource) {
      return &entry;
    }
  }
  return nullptr;
}

const ResourceManager::Entry* ResourceManager::find_entry(
    const ResourceKey resource) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.used && entry.resource == resource) {
      return &entry;
    }
  }
  return nullptr;
}

ResourceManager::Entry* ResourceManager::find_free_entry()
    noexcept {
  for (auto& entry : entries_) {
    if (!entry.used) {
      return &entry;
    }
  }
  return nullptr;
}

ResourceGeneration ResourceManager::next_generation()
    noexcept {
  const auto maximum =
      (std::numeric_limits<ResourceGeneration>::max)();
  if (generation_counter_ == maximum) {
    return 0U;
  }

  ++generation_counter_;
  return generation_counter_;
}

}  // namespace ecu::core::v2::runtime
