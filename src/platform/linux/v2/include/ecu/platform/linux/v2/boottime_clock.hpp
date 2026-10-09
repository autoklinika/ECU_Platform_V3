#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"

namespace ecu::platform::linux::v2 {

class BoottimeClock final
    : public ecu::core::v2::time::IMonotonicClock {
 public:
  static constexpr ecu::core::v2::time::MonotonicClockDomainId
      kDomain{0x4C42544D00000001ULL};

  BoottimeClock(
      ecu::core::v2::time::MonotonicDuration max_read_latency,
      ecu::core::v2::time::MonotonicDuration max_uncertainty) noexcept;

  [[nodiscard]] ecu::core::v2::time::MonotonicClockProperties
  properties() const noexcept override;

  [[nodiscard]] ecu::core::v2::time::MonotonicClockReading
  read() const noexcept override;

 private:
  ecu::core::v2::time::MonotonicClockProperties properties_{};
};

}  // namespace ecu::platform::linux::v2
