#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::runtime {

using ResourceInstance = std::uint32_t;
using ResourceOwnerId = std::uint64_t;
using ResourceGeneration = std::uint64_t;

enum class ResourceClass : std::uint8_t {
  can_channel,
  ethernet_interface,
  diagnostic_channel,
  actuator,
  hardware_io,
  power_domain,
  device_under_test,
  storage,
  custom,
};

struct ResourceKey {
  ResourceClass resource_class{ResourceClass::custom};
  ResourceInstance instance{0U};
};

[[nodiscard]] constexpr bool operator==(
    const ResourceKey lhs,
    const ResourceKey rhs) noexcept {
  return lhs.resource_class == rhs.resource_class &&
         lhs.instance == rhs.instance;
}

[[nodiscard]] constexpr bool operator!=(
    const ResourceKey lhs,
    const ResourceKey rhs) noexcept {
  return !(lhs == rhs);
}

struct ResourceLease {
  ResourceKey resource{};
  ResourceOwnerId owner{0U};
  ResourceGeneration generation{0U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return owner != 0U && generation != 0U;
  }
};

enum class ResourceAcquireStatus : std::uint8_t {
  acquired,
  already_owned,
  busy,
  invalid_argument,
  capacity_exhausted,
  generation_exhausted,
};

struct ResourceAcquireResult {
  ResourceAcquireStatus status{
      ResourceAcquireStatus::invalid_argument};
  ResourceLease lease{};
};

enum class ResourceReleaseStatus : std::uint8_t {
  released,
  invalid_lease,
  not_owner,
  stale_lease,
};

class ResourceManager final {
 public:
  static constexpr std::size_t kMaxResources = 64U;

  // Single-executor runtime contract: acquire/release/lookup are serialized by
  // the owning runtime. No internal threads, locks or dynamic allocation.
  [[nodiscard]] ResourceAcquireResult acquire(
      ResourceKey resource,
      ResourceOwnerId owner) noexcept;

  [[nodiscard]] ResourceReleaseStatus release(
      const ResourceLease& lease) noexcept;

  [[nodiscard]] bool owns(
      ResourceKey resource,
      ResourceOwnerId owner) const noexcept;

  [[nodiscard]] bool owns(
      const ResourceLease& lease) const noexcept;

  [[nodiscard]] ResourceOwnerId current_owner(
      ResourceKey resource) const noexcept;

  [[nodiscard]] std::size_t active_count() const noexcept;

 private:
  struct Entry {
    bool used{false};
    ResourceKey resource{};
    ResourceOwnerId owner{0U};
    ResourceGeneration generation{0U};
  };

  [[nodiscard]] Entry* find_entry(
      ResourceKey resource) noexcept;
  [[nodiscard]] const Entry* find_entry(
      ResourceKey resource) const noexcept;
  [[nodiscard]] Entry* find_free_entry() noexcept;
  [[nodiscard]] ResourceGeneration next_generation() noexcept;

  std::array<Entry, kMaxResources> entries_{};
  ResourceGeneration generation_counter_{0U};
};

}  // namespace ecu::core::v2::runtime
