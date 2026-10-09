#pragma once

#include "ecu/bench/session.hpp"
#include "ecu/core_v2/safety/deadline_watchdog.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <cstdint>

namespace ecu::bench {

struct BenchHostServiceConfig {
  ecu::core::v2::time::MonotonicDuration service_timeout{0};
};

enum class BenchHostServiceStatus : std::uint8_t {
  ok,
  no_action,
  invalid_argument,
  invalid_state,
  session_error,
  watchdog_error,
  deadline_missed,
  clock_fault,
  safe_shutdown_failed,
};

struct BenchHostServiceResult {
  BenchHostServiceStatus status{BenchHostServiceStatus::invalid_state};
  BenchSessionStatus session_status{BenchSessionStatus::invalid_state};
};

struct BenchHostExecutionBudget {
  bool valid{false};
  ecu::core::v2::time::MonotonicDuration max_start_duration{0};
  ecu::core::v2::time::MonotonicDuration max_service_duration{0};
  ecu::core::v2::time::MonotonicDuration max_stop_duration{0};
  ecu::core::v2::time::MonotonicDuration max_recover_duration{0};
};

class BenchSessionHostRuntime final {
 public:
  BenchSessionHostRuntime(
      BenchSession& session,
      const ecu::core::v2::time::IMonotonicClock& clock) noexcept;

  BenchSessionHostRuntime(const BenchSessionHostRuntime&) = delete;
  BenchSessionHostRuntime& operator=(const BenchSessionHostRuntime&) = delete;
  BenchSessionHostRuntime(BenchSessionHostRuntime&&) = delete;
  BenchSessionHostRuntime& operator=(BenchSessionHostRuntime&&) = delete;

  [[nodiscard]] bool configure(
      BenchHostServiceConfig config) noexcept;

  [[nodiscard]] BenchHostServiceResult start() noexcept;
  [[nodiscard]] BenchHostServiceResult service() noexcept;
  [[nodiscard]] BenchHostServiceResult stop() noexcept;
  [[nodiscard]] BenchHostServiceResult recover() noexcept;

  [[nodiscard]] BenchSessionCancelRequestStatus request_cancel(
      ecu::core::v2::runtime::CancellationToken token) noexcept;

  [[nodiscard]] bool configured() const noexcept;
  [[nodiscard]] bool armed() const noexcept;
  [[nodiscard]] BenchHostServiceConfig config() const noexcept;
  [[nodiscard]] BenchHostExecutionBudget execution_budget()
      const noexcept;

 private:
  [[nodiscard]] bool validate_config(
      BenchHostServiceConfig config,
      BenchHostExecutionBudget& budget) const noexcept;
  [[nodiscard]] bool add_duration(
      ecu::core::v2::time::MonotonicDuration left,
      ecu::core::v2::time::MonotonicDuration right,
      ecu::core::v2::time::MonotonicDuration& result) const noexcept;
  [[nodiscard]] BenchHostServiceResult fail_safe_stop(
      BenchHostServiceStatus cause) noexcept;
  [[nodiscard]] BenchHostServiceResult map_terminal_session(
      BenchSessionStatus session_status) noexcept;

  BenchSession& session_;
  ecu::core::v2::safety::DeadlineWatchdog watchdog_;
  ecu::core::v2::time::MonotonicClockProperties clock_properties_{};
  BenchHostServiceConfig config_{};
  BenchHostExecutionBudget budget_{};
  bool configured_{false};
  bool armed_{false};
};

}  // namespace ecu::bench
