#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <cstdint>
#include <limits>

namespace ecu::core::v2::runtime {

using StateRevision = std::uint64_t;
using StateTypeId = std::uint32_t;

struct StateHeader {
  StateRevision revision{0U};
  time::MonotonicTime updated_at{0};
};

[[nodiscard]] constexpr StateRevision next_state_revision(
    const StateRevision current) noexcept {
  return current ==
                 (std::numeric_limits<StateRevision>::max)()
             ? current
             : current + 1U;
}

}  // namespace ecu::core::v2::runtime
