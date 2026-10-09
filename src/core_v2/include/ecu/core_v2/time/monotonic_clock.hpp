#pragma once

#include <chrono>
#include <cstdint>

namespace ecu::core::v2::time {

// Fixed representation for source-level portability across LP64/LLP64 targets.
// These types are Core-internal C++ values, not a wire/IPC ABI.
using MonotonicDuration =
    std::chrono::duration<std::int64_t, std::nano>;
using MonotonicTime = MonotonicDuration;

struct MonotonicClockDomainId {
  std::uint64_t value{0U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return value != 0U;
  }
};

[[nodiscard]] constexpr bool operator==(
    const MonotonicClockDomainId lhs,
    const MonotonicClockDomainId rhs) noexcept {
  return lhs.value == rhs.value;
}

[[nodiscard]] constexpr bool operator!=(
    const MonotonicClockDomainId lhs,
    const MonotonicClockDomainId rhs) noexcept {
  return !(lhs == rhs);
}

enum class MonotonicClockStatus : std::uint8_t {
  ok,
  unavailable,
  discontinuity,
};

struct MonotonicClockProperties {
  MonotonicClockDomainId domain{};
  MonotonicDuration resolution{0};
  MonotonicDuration max_read_latency{0};
  MonotonicDuration max_uncertainty{0};
  bool continuous_across_suspend{false};
};

struct MonotonicClockReading {
  MonotonicClockStatus status{MonotonicClockStatus::unavailable};
  MonotonicClockDomainId domain{};
  MonotonicTime value{0};
  MonotonicDuration uncertainty{0};
};

[[nodiscard]] constexpr bool is_valid_clock_properties(
    const MonotonicClockProperties& properties) noexcept {
  return properties.domain.valid() &&
         properties.resolution.count() > 0 &&
         properties.max_read_latency.count() > 0 &&
         properties.max_uncertainty.count() >= 0 &&
         properties.continuous_across_suspend;
}

[[nodiscard]] constexpr bool is_valid_clock_reading(
    const MonotonicClockReading& reading,
    const MonotonicClockDomainId expected_domain) noexcept {
  return reading.status == MonotonicClockStatus::ok &&
         expected_domain.valid() &&
         reading.domain == expected_domain &&
         reading.value.count() >= 0 &&
         reading.uncertainty.count() >= 0;
}

class IMonotonicClock {
 public:
  virtual ~IMonotonicClock() = default;

  // Properties are stable for the lifetime of the clock object. A clock used
  // for safety deadlines MUST advance across host suspend/resume, have a
  // non-zero domain, finite resolution and declared finite read-latency and
  // uncertainty bounds.
  [[nodiscard]] virtual MonotonicClockProperties properties()
      const noexcept = 0;

  // read() is non-blocking and bounded by properties().max_read_latency. It
  // performs no sleeps, waits for external events or unbounded retries.
  // unavailable/discontinuity are explicit health failures. Successful values
  // never move backwards within one domain.
  [[nodiscard]] virtual MonotonicClockReading read() const noexcept = 0;
};

}  // namespace ecu::core::v2::time
