#include "ecu/core_v2/safety/deadline_watchdog.hpp"

#include <limits>

namespace ecu::core::v2::safety {

DeadlineWatchdog::DeadlineWatchdog(
    const time::IMonotonicClock& clock) noexcept
    : clock_(clock),
      clock_properties_(clock.properties()) {}

DeadlineWatchdogStatus DeadlineWatchdog::arm(
    const time::MonotonicDuration timeout) noexcept {
  if (timeout.count() <= 0) {
    return DeadlineWatchdogStatus::invalid_argument;
  }

  time::MonotonicClockReading reading{};
  if (!read_healthy_clock(reading)) {
    if (state_ == DeadlineWatchdogState::armed) {
      state_ = DeadlineWatchdogState::expired;
    }
    return DeadlineWatchdogStatus::clock_fault;
  }

  if (!precision_sufficient(timeout, reading.uncertainty)) {
    return DeadlineWatchdogStatus::insufficient_clock_precision;
  }

  const auto start = conservative_lower_bound(reading);
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (start.count() > maximum - timeout.count()) {
    return DeadlineWatchdogStatus::invalid_argument;
  }

  timeout_ = timeout;
  deadline_ = start + timeout_;
  state_ = DeadlineWatchdogState::armed;
  return DeadlineWatchdogStatus::ok;
}

DeadlineWatchdogStatus DeadlineWatchdog::kick() noexcept {
  if (state_ == DeadlineWatchdogState::expired) {
    return DeadlineWatchdogStatus::expired;
  }

  if (state_ != DeadlineWatchdogState::armed) {
    return DeadlineWatchdogStatus::not_armed;
  }

  time::MonotonicClockReading reading{};
  if (!read_healthy_clock(reading)) {
    state_ = DeadlineWatchdogState::expired;
    return DeadlineWatchdogStatus::clock_fault;
  }

  if (!precision_sufficient(timeout_, reading.uncertainty)) {
    state_ = DeadlineWatchdogState::expired;
    return DeadlineWatchdogStatus::insufficient_clock_precision;
  }

  if (deadline_reached(reading)) {
    return DeadlineWatchdogStatus::expired;
  }

  const auto start = conservative_lower_bound(reading);
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (start.count() > maximum - timeout_.count()) {
    state_ = DeadlineWatchdogState::expired;
    return DeadlineWatchdogStatus::expired;
  }

  deadline_ = start + timeout_;
  return DeadlineWatchdogStatus::ok;
}

void DeadlineWatchdog::disarm() noexcept {
  state_ = DeadlineWatchdogState::disarmed;
  timeout_ = time::MonotonicDuration{0};
  deadline_ = time::MonotonicTime{0};
}

DeadlineWatchdogStatus DeadlineWatchdog::poll() noexcept {
  if (state_ == DeadlineWatchdogState::expired) {
    return DeadlineWatchdogStatus::expired;
  }

  if (state_ != DeadlineWatchdogState::armed) {
    return DeadlineWatchdogStatus::not_armed;
  }

  time::MonotonicClockReading reading{};
  if (!read_healthy_clock(reading)) {
    state_ = DeadlineWatchdogState::expired;
    return DeadlineWatchdogStatus::clock_fault;
  }

  if (!precision_sufficient(timeout_, reading.uncertainty)) {
    state_ = DeadlineWatchdogState::expired;
    return DeadlineWatchdogStatus::insufficient_clock_precision;
  }

  return deadline_reached(reading)
             ? DeadlineWatchdogStatus::expired
             : DeadlineWatchdogStatus::ok;
}

DeadlineWatchdogState DeadlineWatchdog::state() const noexcept {
  return state_;
}

time::MonotonicTime DeadlineWatchdog::deadline() const noexcept {
  return deadline_;
}

time::MonotonicClockProperties
DeadlineWatchdog::clock_properties() const noexcept {
  return clock_properties_;
}

bool DeadlineWatchdog::read_healthy_clock(
    time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_properties(clock_properties_)) {
    return false;
  }

  reading = clock_.read();
  if (!time::is_valid_clock_reading(
          reading, clock_properties_.domain) ||
      reading.uncertainty > clock_properties_.max_uncertainty) {
    return false;
  }

  if (has_last_observed_ &&
      reading.value < last_observed_) {
    return false;
  }

  last_observed_ = reading.value;
  has_last_observed_ = true;
  return true;
}

bool DeadlineWatchdog::precision_sufficient(
    const time::MonotonicDuration timeout,
    const time::MonotonicDuration uncertainty) const noexcept {
  if (timeout.count() <= 0 ||
      uncertainty.count() < 0 ||
      uncertainty > clock_properties_.max_uncertainty) {
    return false;
  }

  const auto resolution_count = clock_properties_.resolution.count();
  const auto declared_uncertainty_count =
      clock_properties_.max_uncertainty.count();
  const auto guard =
      declared_uncertainty_count > resolution_count
          ? declared_uncertainty_count
          : resolution_count;
  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();

  if (guard <= 0 || guard > maximum / 2) {
    return false;
  }

  // The arm/kick lower bound and poll upper bound can each contribute one
  // guard interval. Require a timeout strictly larger than that ambiguity.
  return timeout.count() > guard * 2;
}

time::MonotonicTime DeadlineWatchdog::conservative_lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }

  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

time::MonotonicTime DeadlineWatchdog::conservative_upper_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (reading.value.count() >
      maximum - reading.uncertainty.count()) {
    return time::MonotonicTime{maximum};
  }

  return time::MonotonicTime{
      reading.value.count() + reading.uncertainty.count()};
}

bool DeadlineWatchdog::deadline_reached(
    const time::MonotonicClockReading& reading) noexcept {
  if (conservative_upper_bound(reading) < deadline_) {
    return false;
  }

  state_ = DeadlineWatchdogState::expired;
  return true;
}

}  // namespace ecu::core::v2::safety
