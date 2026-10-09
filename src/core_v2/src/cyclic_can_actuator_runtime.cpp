#include "ecu/core_v2/actuation/cyclic_can_actuator_runtime.hpp"

#include <limits>

namespace ecu::core::v2::actuation {

namespace {

void saturating_increment(std::uint32_t& value) noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  if (value != maximum) {
    ++value;
  }
}

}  // namespace

CyclicCanActuatorRuntime::CyclicCanActuatorRuntime(
    transport::CanBusRuntime& bus,
    const time::IMonotonicClock& clock,
    ICyclicCanActuatorProgram& program) noexcept
    : bus_(bus),
      clock_(clock),
      program_(program),
      clock_properties_(clock.properties()) {}

bool CyclicCanActuatorRuntime::configure(
    const CyclicCanActuatorConfig& config) noexcept {
  if (state_ != CyclicCanActuatorState::unconfigured &&
      state_ != CyclicCanActuatorState::stopped) {
    return false;
  }

  const auto contract = program_.execution_contract();
  if (!valid_config(config) ||
      !valid_program_contract(contract)) {
    return false;
  }

  config_ = config;
  program_contract_ = contract;
  budget_ = calculate_execution_budget();
  if (!budget_.valid) {
    return false;
  }

  const auto send_budget = bus_.max_send_execution_duration();
  if (send_budget.status != transport::CanStatus::ok) {
    return false;
  }

  time::MonotonicDuration active_send_budget{0};
  if (!multiply_duration(
          send_budget.max_duration,
          program_contract_.max_active_frames,
          active_send_budget)) {
    return false;
  }

  time::MonotonicDuration normal_cycle_budget{0};
  if (!add_duration(
          clock_properties_.max_read_latency,
          program_contract_.max_active_render_duration,
          normal_cycle_budget) ||
      !add_duration(
          normal_cycle_budget,
          active_send_budget,
          normal_cycle_budget) ||
      normal_cycle_budget >= config_.period) {
    return false;
  }

  counters_ = {};
  state_ = CyclicCanActuatorState::stopped;
  status_ = CyclicCanActuatorStatus::ok;
  next_cycle_deadline_ = time::MonotonicTime{0};
  command_deadline_ = time::MonotonicTime{0};
  feedback_deadline_ = time::MonotonicTime{0};
  cycle_sequence_ = 0U;
  configured_ = true;
  interlock_allows_control_ = false;
  command_fresh_ = false;
  feedback_fresh_ = false;
  return true;
}

CyclicCanActuatorStatus CyclicCanActuatorRuntime::start() noexcept {
  if (!configured_ ||
      state_ != CyclicCanActuatorState::stopped) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }

  time::MonotonicClockReading now{};
  if (!read_healthy_clock(now)) {
    saturating_increment(counters_.clock_faults);
    state_ = CyclicCanActuatorState::faulted;
    status_ = CyclicCanActuatorStatus::clock_fault;
    return status_;
  }

  (void)now;
  clear_leases();
  interlock_allows_control_ = false;
  state_ = CyclicCanActuatorState::interlocked;
  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::set_interlock(
    const bool allow_active_control) noexcept {
  if (!configured_ ||
      state_ == CyclicCanActuatorState::unconfigured ||
      state_ == CyclicCanActuatorState::stopped) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }

  if (state_ == CyclicCanActuatorState::faulted) {
    if (!allow_active_control) {
      interlock_allows_control_ = false;
      return CyclicCanActuatorStatus::ok;
    }
    return CyclicCanActuatorStatus::invalid_state;
  }

  if (allow_active_control) {
    if (interlock_allows_control_) {
      status_ = CyclicCanActuatorStatus::no_action;
      return status_;
    }

    interlock_allows_control_ = true;
    if (state_ == CyclicCanActuatorState::interlocked) {
      state_ = CyclicCanActuatorState::ready;
    }
    status_ = CyclicCanActuatorStatus::ok;
    return status_;
  }

  if (!interlock_allows_control_ &&
      state_ == CyclicCanActuatorState::interlocked) {
    status_ = CyclicCanActuatorStatus::no_action;
    return status_;
  }

  interlock_allows_control_ = false;
  if (state_ == CyclicCanActuatorState::active) {
    saturating_increment(counters_.interlock_trips);
    const auto result = perform_safe_stop(
        SafeStopReason::interlock_opened,
        CyclicCanActuatorState::interlocked);
    if (result == CyclicCanActuatorStatus::ok) {
      status_ = CyclicCanActuatorStatus::interlocked;
      return status_;
    }
    return result;
  }

  clear_leases();
  state_ = CyclicCanActuatorState::interlocked;
  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::refresh_command() noexcept {
  if (!configured_ ||
      (state_ != CyclicCanActuatorState::interlocked &&
       state_ != CyclicCanActuatorState::ready &&
       state_ != CyclicCanActuatorState::active)) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }

  time::MonotonicClockReading now{};
  if (!read_healthy_clock(now)) {
    if (state_ == CyclicCanActuatorState::active) {
      return trip_fault(
          SafeStopReason::clock_fault,
          CyclicCanActuatorStatus::clock_fault);
    }

    saturating_increment(counters_.clock_faults);
    state_ = CyclicCanActuatorState::faulted;
    status_ = CyclicCanActuatorStatus::clock_fault;
    return status_;
  }

  if (state_ == CyclicCanActuatorState::active &&
      command_fresh_ &&
      deadline_reached(now, command_deadline_)) {
    return trip_fault(
        SafeStopReason::command_timeout,
        CyclicCanActuatorStatus::command_timeout);
  }

  if (!set_deadline(
          now,
          config_.command_timeout,
          command_deadline_)) {
    if (state_ == CyclicCanActuatorState::active) {
      return trip_fault(
          SafeStopReason::clock_fault,
          CyclicCanActuatorStatus::clock_fault);
    }

    saturating_increment(counters_.clock_faults);
    state_ = CyclicCanActuatorState::faulted;
    status_ = CyclicCanActuatorStatus::clock_fault;
    return status_;
  }

  command_fresh_ = true;
  saturating_increment(counters_.command_refreshes);
  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::activate() noexcept {
  if (!configured_ ||
      state_ != CyclicCanActuatorState::ready) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }
  if (!interlock_allows_control_) {
    state_ = CyclicCanActuatorState::interlocked;
    status_ = CyclicCanActuatorStatus::interlocked;
    return status_;
  }
  if (!command_fresh_) {
    status_ = CyclicCanActuatorStatus::stale_command;
    return status_;
  }

  time::MonotonicClockReading now{};
  if (!read_healthy_clock(now)) {
    return trip_fault(
        SafeStopReason::clock_fault,
        CyclicCanActuatorStatus::clock_fault);
  }
  if (deadline_reached(now, command_deadline_)) {
    return trip_fault(
        SafeStopReason::command_timeout,
        CyclicCanActuatorStatus::command_timeout);
  }

  if (config_.feedback_timeout.count() > 0) {
    if (!set_deadline(
            now,
            config_.feedback_timeout,
            feedback_deadline_)) {
      return trip_fault(
          SafeStopReason::clock_fault,
          CyclicCanActuatorStatus::clock_fault);
    }
    feedback_fresh_ = false;
  }

  state_ = CyclicCanActuatorState::active;
  const auto first_cycle = send_active_cycle(now);
  if (first_cycle == CyclicCanActuatorStatus::program_fault) {
    return trip_fault(
        SafeStopReason::program_fault,
        first_cycle);
  }
  if (first_cycle == CyclicCanActuatorStatus::transport_fault) {
    return trip_fault(
        SafeStopReason::transport_fault,
        first_cycle);
  }

  if (!set_deadline(
          now,
          config_.period,
          next_cycle_deadline_)) {
    return trip_fault(
        SafeStopReason::clock_fault,
        CyclicCanActuatorStatus::clock_fault);
  }

  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::note_valid_feedback() noexcept {
  if (!configured_ ||
      state_ != CyclicCanActuatorState::active) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }
  if (config_.feedback_timeout.count() == 0) {
    status_ = CyclicCanActuatorStatus::no_action;
    return status_;
  }

  time::MonotonicClockReading now{};
  if (!read_healthy_clock(now)) {
    return trip_fault(
        SafeStopReason::clock_fault,
        CyclicCanActuatorStatus::clock_fault);
  }
  if (deadline_reached(now, feedback_deadline_)) {
    return trip_fault(
        SafeStopReason::feedback_timeout,
        CyclicCanActuatorStatus::feedback_timeout);
  }
  if (!set_deadline(
          now,
          config_.feedback_timeout,
          feedback_deadline_)) {
    return trip_fault(
        SafeStopReason::clock_fault,
        CyclicCanActuatorStatus::clock_fault);
  }

  feedback_fresh_ = true;
  saturating_increment(counters_.feedback_refreshes);
  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::service() noexcept {
  if (!configured_ ||
      state_ == CyclicCanActuatorState::unconfigured ||
      state_ == CyclicCanActuatorState::stopped) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }
  if (state_ == CyclicCanActuatorState::faulted) {
    return status_;
  }
  if (state_ != CyclicCanActuatorState::active) {
    status_ = CyclicCanActuatorStatus::no_action;
    return status_;
  }

  time::MonotonicClockReading now{};
  if (!read_healthy_clock(now)) {
    return trip_fault(
        SafeStopReason::clock_fault,
        CyclicCanActuatorStatus::clock_fault);
  }

  if (!command_fresh_ ||
      deadline_reached(now, command_deadline_)) {
    return trip_fault(
        SafeStopReason::command_timeout,
        CyclicCanActuatorStatus::command_timeout);
  }

  if (config_.feedback_timeout.count() > 0 &&
      deadline_reached(now, feedback_deadline_)) {
    return trip_fault(
        SafeStopReason::feedback_timeout,
        CyclicCanActuatorStatus::feedback_timeout);
  }

  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (next_cycle_deadline_.count() >
      maximum - config_.max_lateness.count()) {
    return trip_fault(
        SafeStopReason::cadence_missed,
        CyclicCanActuatorStatus::cadence_missed);
  }

  const auto latest_allowed =
      next_cycle_deadline_ + config_.max_lateness;
  if (upper_bound(now) > latest_allowed) {
    return trip_fault(
        SafeStopReason::cadence_missed,
        CyclicCanActuatorStatus::cadence_missed);
  }

  if (lower_bound(now) < next_cycle_deadline_) {
    status_ = CyclicCanActuatorStatus::no_action;
    return status_;
  }

  const auto cycle = send_active_cycle(now);
  if (cycle == CyclicCanActuatorStatus::program_fault) {
    return trip_fault(
        SafeStopReason::program_fault,
        cycle);
  }
  if (cycle == CyclicCanActuatorStatus::transport_fault) {
    return trip_fault(
        SafeStopReason::transport_fault,
        cycle);
  }

  if (next_cycle_deadline_.count() >
      maximum - config_.period.count()) {
    return trip_fault(
        SafeStopReason::cadence_missed,
        CyclicCanActuatorStatus::cadence_missed);
  }
  next_cycle_deadline_ += config_.period;

  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus CyclicCanActuatorRuntime::stop() noexcept {
  if (!configured_) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }
  if (state_ == CyclicCanActuatorState::stopped) {
    status_ = CyclicCanActuatorStatus::no_action;
    return status_;
  }
  if (state_ == CyclicCanActuatorState::faulted ||
      state_ == CyclicCanActuatorState::unconfigured) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }

  if (state_ == CyclicCanActuatorState::active) {
    const auto result = perform_safe_stop(
        SafeStopReason::explicit_stop,
        CyclicCanActuatorState::stopped);
    if (result != CyclicCanActuatorStatus::ok) {
      return result;
    }
  } else {
    clear_leases();
    state_ = CyclicCanActuatorState::stopped;
  }

  interlock_allows_control_ = false;
  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::reset_fault() noexcept {
  if (!configured_ ||
      state_ != CyclicCanActuatorState::faulted) {
    status_ = CyclicCanActuatorStatus::invalid_state;
    return status_;
  }
  if (interlock_allows_control_) {
    status_ = CyclicCanActuatorStatus::interlocked;
    return status_;
  }

  clear_leases();
  state_ = CyclicCanActuatorState::stopped;
  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorState
CyclicCanActuatorRuntime::state() const noexcept {
  return state_;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::status() const noexcept {
  return status_;
}

CyclicCanActuatorCounters
CyclicCanActuatorRuntime::counters() const noexcept {
  return counters_;
}

time::MonotonicTime
CyclicCanActuatorRuntime::next_service_deadline()
    const noexcept {
  return state_ == CyclicCanActuatorState::active
             ? next_cycle_deadline_
             : time::MonotonicTime{0};
}

CyclicCanActuatorExecutionBudget
CyclicCanActuatorRuntime::execution_budget() const noexcept {
  return budget_;
}

bool CyclicCanActuatorRuntime::valid_config(
    const CyclicCanActuatorConfig& config) const noexcept {
  if (!time::is_valid_clock_properties(clock_properties_) ||
      config.period.count() <= 0 ||
      config.max_lateness.count() <= 0 ||
      config.max_lateness >= config.period ||
      config.command_timeout.count() <= 0 ||
      config.feedback_timeout.count() < 0) {
    return false;
  }

  return precision_sufficient(config.period) &&
         precision_sufficient(config.max_lateness) &&
         precision_sufficient(config.command_timeout) &&
         (config.feedback_timeout.count() == 0 ||
          precision_sufficient(config.feedback_timeout));
}

bool CyclicCanActuatorRuntime::valid_program_contract(
    const CyclicCanActuatorProgramContract& contract) const noexcept {
  return contract.max_active_frames > 0U &&
         contract.max_active_frames <=
             CyclicCanFrameBatch::kCapacity &&
         contract.max_safe_stop_frames <=
             CyclicCanFrameBatch::kCapacity &&
         contract.max_active_render_duration.count() > 0 &&
         contract.max_safe_stop_render_duration.count() > 0;
}

bool CyclicCanActuatorRuntime::read_healthy_clock(
    time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_properties(clock_properties_)) {
    return false;
  }

  reading = clock_.read();
  if (!time::is_valid_clock_reading(
          reading,
          clock_properties_.domain) ||
      reading.uncertainty >
          clock_properties_.max_uncertainty) {
    return false;
  }

  if (has_last_observed_time_ &&
      reading.value < last_observed_time_) {
    return false;
  }

  last_observed_time_ = reading.value;
  has_last_observed_time_ = true;
  return true;
}

bool CyclicCanActuatorRuntime::precision_sufficient(
    const time::MonotonicDuration duration) const noexcept {
  if (duration.count() <= 0) {
    return false;
  }

  const auto resolution =
      clock_properties_.resolution.count();
  const auto uncertainty =
      clock_properties_.max_uncertainty.count();
  const auto guard =
      uncertainty > resolution ? uncertainty : resolution;
  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();

  if (guard <= 0 || guard > maximum / 2) {
    return false;
  }

  return duration.count() > guard * 2;
}

time::MonotonicTime CyclicCanActuatorRuntime::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }

  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

time::MonotonicTime CyclicCanActuatorRuntime::upper_bound(
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

bool CyclicCanActuatorRuntime::set_deadline(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration duration,
    time::MonotonicTime& deadline) const noexcept {
  if (duration.count() <= 0) {
    return false;
  }

  const auto start = lower_bound(reading);
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (start.count() > maximum - duration.count()) {
    return false;
  }

  deadline = start + duration;
  return true;
}

bool CyclicCanActuatorRuntime::deadline_reached(
    const time::MonotonicClockReading& reading,
    const time::MonotonicTime deadline) const noexcept {
  return upper_bound(reading) >= deadline;
}

bool CyclicCanActuatorRuntime::add_duration(
    const time::MonotonicDuration left,
    const time::MonotonicDuration right,
    time::MonotonicDuration& result) const noexcept {
  if (left.count() < 0 || right.count() < 0) {
    return false;
  }

  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();
  if (left.count() > maximum - right.count()) {
    return false;
  }

  result = time::MonotonicDuration{
      left.count() + right.count()};
  return true;
}

bool CyclicCanActuatorRuntime::multiply_duration(
    const time::MonotonicDuration duration,
    const std::uint8_t count,
    time::MonotonicDuration& result) const noexcept {
  if (duration.count() < 0) {
    return false;
  }
  if (count == 0U) {
    result = time::MonotonicDuration{0};
    return true;
  }

  const auto maximum =
      (std::numeric_limits<time::MonotonicDuration::rep>::max)();
  const auto multiplier =
      static_cast<time::MonotonicDuration::rep>(count);
  if (duration.count() > maximum / multiplier) {
    return false;
  }

  result = time::MonotonicDuration{
      duration.count() * multiplier};
  return true;
}

CyclicCanActuatorExecutionBudget
CyclicCanActuatorRuntime::calculate_execution_budget()
    const noexcept {
  CyclicCanActuatorExecutionBudget result{};
  const auto send_budget = bus_.max_send_execution_duration();
  if (send_budget.status != transport::CanStatus::ok) {
    return result;
  }

  time::MonotonicDuration active_send{0};
  time::MonotonicDuration safe_send{0};
  if (!multiply_duration(
          send_budget.max_duration,
          program_contract_.max_active_frames,
          active_send) ||
      !multiply_duration(
          send_budget.max_duration,
          program_contract_.max_safe_stop_frames,
          safe_send)) {
    return result;
  }

  time::MonotonicDuration safe_total{0};
  if (!add_duration(
          program_contract_.max_safe_stop_render_duration,
          safe_send,
          safe_total)) {
    return result;
  }

  time::MonotonicDuration active_total{0};
  if (!add_duration(
          program_contract_.max_active_render_duration,
          active_send,
          active_total)) {
    return result;
  }

  time::MonotonicDuration activate{0};
  if (!add_duration(
          clock_properties_.max_read_latency,
          active_total,
          activate) ||
      !add_duration(activate, safe_total, activate)) {
    return result;
  }

  time::MonotonicDuration service{0};
  if (!add_duration(
          clock_properties_.max_read_latency,
          active_total,
          service) ||
      !add_duration(service, safe_total, service)) {
    return result;
  }

  result.valid = true;
  result.max_service_duration = service;
  result.max_activate_duration = activate;
  result.max_safe_stop_duration = safe_total;
  return result;
}

bool CyclicCanActuatorRuntime::validate_batch(
    const CyclicCanFrameBatch& batch,
    const std::uint8_t declared_max,
    const bool active_batch) const noexcept {
  if (batch.count > declared_max ||
      batch.count > CyclicCanFrameBatch::kCapacity ||
      (active_batch && batch.count == 0U)) {
    return false;
  }

  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(batch.count);
       ++index) {
    if (!transport::is_valid_can_frame(batch.frames[index])) {
      return false;
    }
  }
  return true;
}

transport::CanStatus CyclicCanActuatorRuntime::send_batch(
    const CyclicCanFrameBatch& batch,
    const bool safe_stop_batch) noexcept {
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(batch.count);
       ++index) {
    const auto send_status = bus_.send(batch.frames[index]);
    if (send_status != transport::CanStatus::ok) {
      return send_status;
    }

    if (safe_stop_batch) {
      saturating_increment(counters_.safe_stop_frames_sent);
    } else {
      saturating_increment(counters_.active_frames_sent);
    }
  }

  return transport::CanStatus::ok;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::send_active_cycle(
    const time::MonotonicClockReading& now) noexcept {
  (void)now;
  CyclicCanFrameBatch batch{};
  const auto rendered =
      program_.render_active_cycle(cycle_sequence_, batch);
  if (rendered != ActuatorProgramStatus::ok ||
      !validate_batch(
          batch,
          program_contract_.max_active_frames,
          true)) {
    return CyclicCanActuatorStatus::program_fault;
  }

  if (send_batch(batch, false) != transport::CanStatus::ok) {
    return CyclicCanActuatorStatus::transport_fault;
  }

  saturating_increment(counters_.active_cycles_sent);
  ++cycle_sequence_;
  return CyclicCanActuatorStatus::ok;
}

CyclicCanActuatorStatus
CyclicCanActuatorRuntime::perform_safe_stop(
    const SafeStopReason reason,
    const CyclicCanActuatorState success_state) noexcept {
  saturating_increment(counters_.safe_stop_events);

  CyclicCanFrameBatch batch{};
  const auto rendered =
      program_.render_safe_stop(reason, batch);
  if (rendered != ActuatorProgramStatus::ok ||
      !validate_batch(
          batch,
          program_contract_.max_safe_stop_frames,
          false)) {
    saturating_increment(counters_.safe_stop_failures);
    clear_leases();
    state_ = CyclicCanActuatorState::faulted;
    status_ = CyclicCanActuatorStatus::safe_stop_failed;
    return status_;
  }

  if (send_batch(batch, true) != transport::CanStatus::ok) {
    saturating_increment(counters_.safe_stop_failures);
    clear_leases();
    state_ = CyclicCanActuatorState::faulted;
    status_ = CyclicCanActuatorStatus::safe_stop_failed;
    return status_;
  }

  clear_leases();
  state_ = success_state;
  status_ = CyclicCanActuatorStatus::ok;
  return status_;
}

CyclicCanActuatorStatus CyclicCanActuatorRuntime::trip_fault(
    const SafeStopReason reason,
    const CyclicCanActuatorStatus fault_status) noexcept {
  switch (reason) {
    case SafeStopReason::explicit_stop:
      break;
    case SafeStopReason::interlock_opened:
      saturating_increment(counters_.interlock_trips);
      break;
    case SafeStopReason::command_timeout:
      saturating_increment(counters_.command_timeouts);
      break;
    case SafeStopReason::feedback_timeout:
      saturating_increment(counters_.feedback_timeouts);
      break;
    case SafeStopReason::cadence_missed:
      saturating_increment(counters_.cadence_misses);
      break;
    case SafeStopReason::clock_fault:
      saturating_increment(counters_.clock_faults);
      break;
    case SafeStopReason::program_fault:
      saturating_increment(counters_.program_faults);
      break;
    case SafeStopReason::transport_fault:
      saturating_increment(counters_.transport_faults);
      break;
  }

  const auto safe_stop = perform_safe_stop(
      reason,
      CyclicCanActuatorState::faulted);
  state_ = CyclicCanActuatorState::faulted;
  if (safe_stop != CyclicCanActuatorStatus::ok) {
    return safe_stop;
  }

  status_ = fault_status;
  return status_;
}

void CyclicCanActuatorRuntime::clear_leases() noexcept {
  next_cycle_deadline_ = time::MonotonicTime{0};
  command_deadline_ = time::MonotonicTime{0};
  feedback_deadline_ = time::MonotonicTime{0};
  cycle_sequence_ = 0U;
  command_fresh_ = false;
  feedback_fresh_ = false;
}

}  // namespace ecu::core::v2::actuation
