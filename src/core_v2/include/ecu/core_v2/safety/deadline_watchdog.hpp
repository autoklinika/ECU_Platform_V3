#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <cstdint>

namespace ecu::core::v2::safety {

enum class DeadlineWatchdogState : std::uint8_t {
  disarmed,
  armed,
  expired,
};

enum class DeadlineWatchdogStatus : std::uint8_t {
  ok,
  invalid_argument,
  not_armed,
  expired,
  clock_fault,
  insufficient_clock_precision,
};

class DeadlineWatchdog {
 public:
  // clock MUST outlive this object. The watchdog is single-executor and not
  // internally thread-safe. arm/kick/poll/disarm/state/deadline are serialized
  // by the owning runtime.
  explicit DeadlineWatchdog(
      const time::IMonotonicClock& clock) noexcept;

  DeadlineWatchdog(const DeadlineWatchdog&) = delete;
  DeadlineWatchdog& operator=(const DeadlineWatchdog&) = delete;
  DeadlineWatchdog(DeadlineWatchdog&&) = delete;
  DeadlineWatchdog& operator=(DeadlineWatchdog&&) = delete;

  // A valid arm explicitly rearms from disarmed, armed or expired. Invalid
  // timeout/insufficient clock precision leave the existing state/deadline
  // unchanged. Unhealthy/discontinuous clock input expires an already-armed
  // watchdog fail-closed.
  [[nodiscard]] DeadlineWatchdogStatus arm(
      time::MonotonicDuration timeout) noexcept;
  [[nodiscard]] DeadlineWatchdogStatus kick() noexcept;
  void disarm() noexcept;

  // poll() is the expiry observation point and must be scheduled within the
  // safety service period. state() is cached and performs no clock read.
  [[nodiscard]] DeadlineWatchdogStatus poll() noexcept;
  [[nodiscard]] DeadlineWatchdogState state() const noexcept;
  [[nodiscard]] time::MonotonicTime deadline() const noexcept;
  [[nodiscard]] time::MonotonicClockProperties clock_properties()
      const noexcept;

 private:
  [[nodiscard]] bool read_healthy_clock(
      time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool precision_sufficient(
      time::MonotonicDuration timeout,
      time::MonotonicDuration uncertainty) const noexcept;
  [[nodiscard]] time::MonotonicTime conservative_lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;
  [[nodiscard]] time::MonotonicTime conservative_upper_bound(
      const time::MonotonicClockReading& reading) const noexcept;
  [[nodiscard]] bool deadline_reached(
      const time::MonotonicClockReading& reading) noexcept;

  const time::IMonotonicClock& clock_;
  time::MonotonicClockProperties clock_properties_{};
  DeadlineWatchdogState state_{DeadlineWatchdogState::disarmed};
  time::MonotonicDuration timeout_{0};
  time::MonotonicTime deadline_{0};
  time::MonotonicTime last_observed_{0};
  bool has_last_observed_{false};
};

}  // namespace ecu::core::v2::safety
