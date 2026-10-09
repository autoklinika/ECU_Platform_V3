#include "ecu/platform/linux/v2/boottime_clock.hpp"

#include <ctime>
#include <limits>

namespace ecu::platform::linux::v2 {
namespace {

[[nodiscard]] bool timespec_to_duration(
    const timespec& value,
    ecu::core::v2::time::MonotonicDuration& result) noexcept {
  if (value.tv_sec < 0 || value.tv_nsec < 0 ||
      value.tv_nsec >= 1000000000L) {
    return false;
  }

  constexpr auto kNanosPerSecond = 1000000000LL;
  const auto maximum =
      (std::numeric_limits<std::int64_t>::max)();

  const auto seconds =
      static_cast<std::int64_t>(value.tv_sec);
  if (seconds > maximum / kNanosPerSecond) {
    return false;
  }

  const auto base = seconds * kNanosPerSecond;
  const auto nanos =
      static_cast<std::int64_t>(value.tv_nsec);
  if (base > maximum - nanos) {
    return false;
  }

  result =
      ecu::core::v2::time::MonotonicDuration{base + nanos};
  return true;
}

}  // namespace

BoottimeClock::BoottimeClock(
    const ecu::core::v2::time::MonotonicDuration max_read_latency,
    const ecu::core::v2::time::MonotonicDuration max_uncertainty)
    noexcept {
  timespec resolution{};
  ecu::core::v2::time::MonotonicDuration converted{0};

  if (max_read_latency.count() <= 0 ||
      max_uncertainty.count() < 0 ||
      ::clock_getres(CLOCK_BOOTTIME, &resolution) != 0 ||
      !timespec_to_duration(resolution, converted)) {
    return;
  }

  if (converted.count() <= 0) {
    converted = ecu::core::v2::time::MonotonicDuration{1};
  }

  properties_ = {
      kDomain,
      converted,
      max_read_latency,
      max_uncertainty,
      true};
}

ecu::core::v2::time::MonotonicClockProperties
BoottimeClock::properties() const noexcept {
  return properties_;
}

ecu::core::v2::time::MonotonicClockReading
BoottimeClock::read() const noexcept {
  using namespace ecu::core::v2::time;

  if (!is_valid_clock_properties(properties_)) {
    return {
        MonotonicClockStatus::unavailable,
        {},
        {},
        {}};
  }

  timespec now{};
  MonotonicDuration converted{0};
  if (::clock_gettime(CLOCK_BOOTTIME, &now) != 0 ||
      !timespec_to_duration(now, converted)) {
    return {
        MonotonicClockStatus::unavailable,
        properties_.domain,
        {},
        properties_.max_uncertainty};
  }

  return {
      MonotonicClockStatus::ok,
      properties_.domain,
      converted,
      properties_.max_uncertainty};
}

}  // namespace ecu::platform::linux::v2
