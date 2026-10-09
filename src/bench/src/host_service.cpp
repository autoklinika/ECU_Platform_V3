#include "ecu/bench/host_service.hpp"

#include <limits>

namespace ecu::bench {

BenchSessionHostRuntime::BenchSessionHostRuntime(
    BenchSession& session,
    const ecu::core::v2::time::IMonotonicClock& clock) noexcept
    : session_(session),
      watchdog_(clock),
      clock_properties_(clock.properties()) {}

bool BenchSessionHostRuntime::configure(
    const BenchHostServiceConfig config) noexcept {
  if (armed_ || session_.state() == BenchSessionState::running) {
    return false;
  }

  BenchHostExecutionBudget budget{};
  if (!validate_config(config, budget)) {
    return false;
  }

  config_ = config;
  budget_ = budget;
  configured_ = true;
  return true;
}

BenchHostServiceResult BenchSessionHostRuntime::start() noexcept {
  if (!configured_ || armed_ ||
      session_.state() != BenchSessionState::ready) {
    return {
        BenchHostServiceStatus::invalid_state,
        session_.status()};
  }

  const auto session_status = session_.start();
  if (session_status != BenchSessionStatus::ok) {
    return {
        BenchHostServiceStatus::session_error,
        session_status};
  }

  const auto watchdog_status =
      watchdog_.arm(config_.service_timeout);
  if (watchdog_status ==
      ecu::core::v2::safety::DeadlineWatchdogStatus::ok) {
    armed_ = true;
    return {BenchHostServiceStatus::ok, session_status};
  }

  return fail_safe_stop(
      watchdog_status ==
              ecu::core::v2::safety::DeadlineWatchdogStatus::clock_fault
          ? BenchHostServiceStatus::clock_fault
          : BenchHostServiceStatus::watchdog_error);
}

BenchHostServiceResult BenchSessionHostRuntime::service() noexcept {
  if (!configured_ || !armed_ ||
      session_.state() != BenchSessionState::running) {
    return {
        BenchHostServiceStatus::invalid_state,
        session_.status()};
  }

  const auto poll = watchdog_.poll();
  if (poll ==
      ecu::core::v2::safety::DeadlineWatchdogStatus::expired) {
    return fail_safe_stop(
        BenchHostServiceStatus::deadline_missed);
  }
  if (poll ==
      ecu::core::v2::safety::DeadlineWatchdogStatus::clock_fault) {
    return fail_safe_stop(
        BenchHostServiceStatus::clock_fault);
  }
  if (poll !=
      ecu::core::v2::safety::DeadlineWatchdogStatus::ok) {
    return fail_safe_stop(
        BenchHostServiceStatus::watchdog_error);
  }

  const auto session_status = session_.service();
  if (session_.state() != BenchSessionState::running) {
    watchdog_.disarm();
    armed_ = false;
    return map_terminal_session(session_status);
  }

  if (session_status != BenchSessionStatus::ok &&
      session_status != BenchSessionStatus::no_action) {
    return fail_safe_stop(
        BenchHostServiceStatus::session_error);
  }

  const auto kick = watchdog_.kick();
  if (kick ==
      ecu::core::v2::safety::DeadlineWatchdogStatus::ok) {
    return {
        session_status == BenchSessionStatus::no_action
            ? BenchHostServiceStatus::no_action
            : BenchHostServiceStatus::ok,
        session_status};
  }
  if (kick ==
      ecu::core::v2::safety::DeadlineWatchdogStatus::expired) {
    return fail_safe_stop(
        BenchHostServiceStatus::deadline_missed);
  }
  if (kick ==
      ecu::core::v2::safety::DeadlineWatchdogStatus::clock_fault) {
    return fail_safe_stop(
        BenchHostServiceStatus::clock_fault);
  }

  return fail_safe_stop(
      BenchHostServiceStatus::watchdog_error);
}

BenchHostServiceResult BenchSessionHostRuntime::stop() noexcept {
  if (!configured_) {
    return {
        BenchHostServiceStatus::invalid_state,
        session_.status()};
  }

  const auto session_status = session_.stop();
  watchdog_.disarm();
  armed_ = false;

  if (session_status == BenchSessionStatus::ok) {
    return {BenchHostServiceStatus::ok, session_status};
  }
  if (session_status == BenchSessionStatus::no_action) {
    return {BenchHostServiceStatus::no_action, session_status};
  }
  if (session_status == BenchSessionStatus::safe_shutdown_failed) {
    return {
        BenchHostServiceStatus::safe_shutdown_failed,
        session_status};
  }
  return {
      BenchHostServiceStatus::session_error,
      session_status};
}

BenchHostServiceResult BenchSessionHostRuntime::recover() noexcept {
  if (!configured_) {
    return {
        BenchHostServiceStatus::invalid_state,
        session_.status()};
  }

  watchdog_.disarm();
  armed_ = false;
  const auto session_status = session_.recover();

  if (session_status == BenchSessionStatus::ok) {
    return {BenchHostServiceStatus::ok, session_status};
  }
  if (session_status == BenchSessionStatus::safe_shutdown_failed) {
    return {
        BenchHostServiceStatus::safe_shutdown_failed,
        session_status};
  }
  return {
      BenchHostServiceStatus::session_error,
      session_status};
}

BenchSessionCancelRequestStatus
BenchSessionHostRuntime::request_cancel(
    const ecu::core::v2::runtime::CancellationToken token) noexcept {
  if (!configured_) {
    return BenchSessionCancelRequestStatus::no_active_operation;
  }
  return session_.request_cancel(token);
}

bool BenchSessionHostRuntime::configured() const noexcept {
  return configured_;
}

bool BenchSessionHostRuntime::armed() const noexcept {
  return armed_;
}

BenchHostServiceConfig
BenchSessionHostRuntime::config() const noexcept {
  return config_;
}

BenchHostExecutionBudget
BenchSessionHostRuntime::execution_budget() const noexcept {
  return budget_;
}

bool BenchSessionHostRuntime::validate_config(
    const BenchHostServiceConfig config,
    BenchHostExecutionBudget& budget) const noexcept {
  budget = {};

  if (config.service_timeout.count() <= 0 ||
      !ecu::core::v2::time::is_valid_clock_properties(
          clock_properties_)) {
    return false;
  }

  const auto session_budget = session_.execution_budget();
  if (!session_budget.valid) {
    return false;
  }

  const auto worse_precision =
      clock_properties_.resolution >
              clock_properties_.max_uncertainty
          ? clock_properties_.resolution
          : clock_properties_.max_uncertainty;

  ecu::core::v2::time::MonotonicDuration twice_precision{0};
  if (!add_duration(
          worse_precision,
          worse_precision,
          twice_precision) ||
      config.service_timeout <= twice_precision) {
    return false;
  }

  ecu::core::v2::time::MonotonicDuration normal_service{0};
  if (!add_duration(
          clock_properties_.max_read_latency,
          session_budget.max_service_duration,
          normal_service) ||
      !add_duration(
          normal_service,
          clock_properties_.max_read_latency,
          normal_service) ||
      config.service_timeout <= normal_service) {
    return false;
  }

  ecu::core::v2::time::MonotonicDuration start{
      session_budget.max_start_duration};
  if (!add_duration(
          start,
          clock_properties_.max_read_latency,
          start) ||
      !add_duration(
          start,
          session_budget.max_stop_duration,
          start)) {
    return false;
  }

  ecu::core::v2::time::MonotonicDuration service{
      normal_service};
  if (!add_duration(
          service,
          session_budget.max_stop_duration,
          service)) {
    return false;
  }

  budget.valid = true;
  budget.max_start_duration = start;
  budget.max_service_duration = service;
  budget.max_stop_duration = session_budget.max_stop_duration;
  budget.max_recover_duration =
      session_budget.max_recover_duration;
  return true;
}

bool BenchSessionHostRuntime::add_duration(
    const ecu::core::v2::time::MonotonicDuration left,
    const ecu::core::v2::time::MonotonicDuration right,
    ecu::core::v2::time::MonotonicDuration& result) const noexcept {
  if (left.count() < 0 || right.count() < 0) {
    return false;
  }

  const auto maximum =
      (std::numeric_limits<
          ecu::core::v2::time::MonotonicDuration::rep>::max)();
  if (left.count() > maximum - right.count()) {
    return false;
  }

  result = ecu::core::v2::time::MonotonicDuration{
      left.count() + right.count()};
  return true;
}

BenchHostServiceResult BenchSessionHostRuntime::fail_safe_stop(
    const BenchHostServiceStatus cause) noexcept {
  const auto stopped = session_.stop();
  watchdog_.disarm();
  armed_ = false;

  if (stopped != BenchSessionStatus::ok &&
      stopped != BenchSessionStatus::no_action) {
    return {
        BenchHostServiceStatus::safe_shutdown_failed,
        stopped};
  }

  return {cause, stopped};
}

BenchHostServiceResult
BenchSessionHostRuntime::map_terminal_session(
    const BenchSessionStatus session_status) noexcept {
  if (session_status == BenchSessionStatus::cancelled) {
    return {BenchHostServiceStatus::ok, session_status};
  }
  if (session_status == BenchSessionStatus::safe_shutdown_failed) {
    return {
        BenchHostServiceStatus::safe_shutdown_failed,
        session_status};
  }
  if (session_status == BenchSessionStatus::ok ||
      session_status == BenchSessionStatus::no_action) {
    return {
        BenchHostServiceStatus::no_action,
        session_status};
  }
  return {
      BenchHostServiceStatus::session_error,
      session_status};
}

}  // namespace ecu::bench
