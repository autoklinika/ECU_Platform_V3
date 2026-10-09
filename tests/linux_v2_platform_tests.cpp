#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/can_types.hpp"
#include "ecu/platform/linux/v2/boottime_clock.hpp"
#include "ecu/platform/linux/v2/socketcan_adapter.hpp"
#include "ecu/platform/linux/v2/socketcan_link_info.hpp"

#include <chrono>
#include <iostream>

namespace {

namespace core = ecu::core::v2;
namespace platform = ecu::platform::linux::v2;

int require(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

[[nodiscard]] core::transport::CanDriverExecutionContract
execution_contract() noexcept {
  return {
      std::chrono::milliseconds{1},
      std::chrono::milliseconds{1},
      std::chrono::milliseconds{100},
      std::chrono::milliseconds{20},
      std::chrono::milliseconds{5},
      std::chrono::milliseconds{5},
      std::chrono::milliseconds{1}};
}

}  // namespace

int main() {
  int failures = 0;

  platform::BoottimeClock clock{
      std::chrono::milliseconds{1},
      std::chrono::microseconds{100}};

  const auto properties = clock.properties();
  failures += require(
      core::time::is_valid_clock_properties(properties),
      "CLOCK_BOOTTIME properties satisfy Core V2 clock contract");
  failures += require(
      properties.domain == platform::BoottimeClock::kDomain &&
          properties.continuous_across_suspend,
      "CLOCK_BOOTTIME uses stable suspend-aware domain");

  const auto first = clock.read();
  const auto second = clock.read();
  failures += require(
      core::time::is_valid_clock_reading(first, properties.domain) &&
          core::time::is_valid_clock_reading(second, properties.domain) &&
          second.value >= first.value,
      "CLOCK_BOOTTIME readings are valid and monotonic");

  const auto missing =
      platform::query_socketcan_link(
          "__ecu_platform_missing_can__",
          20);
  failures += require(
      missing.status ==
          platform::SocketCanLinkQueryStatus::not_found,
      "missing SocketCAN interface fails closed");

  const auto loopback =
      platform::query_socketcan_link("lo", 20);
  failures += require(
      loopback.status ==
          platform::SocketCanLinkQueryStatus::not_can,
      "non-CAN Linux interface is rejected");

  platform::SocketCanAdapter invalid_adapter{
      "lo",
      clock,
      execution_contract()};
  failures += require(
      !invalid_adapter.valid() &&
          !invalid_adapter.physical_channel_id().valid(),
      "SocketCAN adapter rejects non-CAN interface");

  if (failures != 0) {
    return 1;
  }

  std::cout << "LINUX_V2_PLATFORM_TESTS=PASS\n";
  return 0;
}
