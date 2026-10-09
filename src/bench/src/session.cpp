#include "ecu/bench/session.hpp"

#include <limits>

namespace ecu::bench {

namespace {

void saturating_increment(std::uint32_t& value) noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  if (value != maximum) {
    ++value;
  }
}

[[nodiscard]] bool component_ok(
    const BenchComponentStatus status) noexcept {
  return status == BenchComponentStatus::ok ||
         status == BenchComponentStatus::no_action;
}

class ScopedBusy final {
 public:
  explicit ScopedBusy(bool& busy) noexcept : busy_(busy) {
    busy_ = true;
  }

  ~ScopedBusy() {
    busy_ = false;
  }

  ScopedBusy(const ScopedBusy&) = delete;
  ScopedBusy& operator=(const ScopedBusy&) = delete;

 private:
  bool& busy_;
};

}  // namespace

BenchSession::BenchSession(
    ecu::core::v2::runtime::ResourceManager& resources,
    const ecu::core::v2::runtime::DutRegistry& duts,
    IDutSessionEndpoint& endpoint,
    IBenchElectricalControl* electrical,
    IEnvironmentSession* environment,
    IBenchSessionEventPublisher* events) noexcept
    : resources_(resources),
      duts_(duts),
      endpoint_(endpoint),
      electrical_(electrical),
      environment_(environment),
      events_(events) {}

BenchSessionConfigValidationStatus
BenchSession::validate_configuration(
    const BenchSessionConfig& config) const noexcept {
  if (config.session_owner == 0U) {
    return BenchSessionConfigValidationStatus::invalid_session_owner;
  }
  if (!config.dut.valid()) {
    return BenchSessionConfigValidationStatus::invalid_dut_handle;
  }
  if (duts_.configuration_state() !=
      ecu::core::v2::runtime::ConfigurationState::frozen) {
    return BenchSessionConfigValidationStatus::dut_registry_not_frozen;
  }
  if (config.additional_resource_count >
      config.additional_resources.size()) {
    return BenchSessionConfigValidationStatus::too_many_resources;
  }

  const auto* dut = duts_.get(config.dut);
  if (dut == nullptr) {
    return BenchSessionConfigValidationStatus::dut_not_found;
  }

  for (std::size_t index = 0U;
       index <
           static_cast<std::size_t>(
               config.additional_resource_count);
       ++index) {
    const auto resource = config.additional_resources[index];
    if (resource.resource_class ==
        ecu::core::v2::runtime::ResourceClass::device_under_test) {
      return BenchSessionConfigValidationStatus::forbidden_dut_resource;
    }

    for (std::size_t other = index + 1U;
         other <
             static_cast<std::size_t>(
                 config.additional_resource_count);
         ++other) {
      if (resource == config.additional_resources[other]) {
        return BenchSessionConfigValidationStatus::duplicate_resource;
      }
    }
  }

  if (!config.use_electrical_control) {
    if (config.run_electrical_state != BenchElectricalState{} ||
        config.use_wake_pulse ||
        config.wake_pulse_width.count() != 0 ||
        config.verify_electrical_state) {
      return BenchSessionConfigValidationStatus::
          electrical_request_without_control;
    }
  } else {
    if (electrical_ == nullptr) {
      return BenchSessionConfigValidationStatus::
          electrical_control_unavailable;
    }
    if (!is_valid_electrical_state(
            config.run_electrical_state)) {
      return BenchSessionConfigValidationStatus::
          invalid_electrical_state;
    }

    const auto capabilities = electrical_->capabilities();

    if ((config.run_electrical_state.power &&
         !has_bench_electrical_capability(
             capabilities,
             BenchElectricalCapability::power)) ||
        (config.run_electrical_state.ignition &&
         !has_bench_electrical_capability(
             capabilities,
             BenchElectricalCapability::ignition)) ||
        (config.run_electrical_state.wake &&
         !has_bench_electrical_capability(
             capabilities,
             BenchElectricalCapability::wake_level))) {
      return BenchSessionConfigValidationStatus::
          electrical_capability_missing;
    }

    if (config.use_wake_pulse) {
      if (!config.run_electrical_state.power ||
          config.wake_pulse_width.count() <= 0) {
        return BenchSessionConfigValidationStatus::invalid_wake_pulse;
      }
      if (!has_bench_electrical_capability(
              capabilities,
              BenchElectricalCapability::wake_pulse)) {
        return BenchSessionConfigValidationStatus::
            electrical_capability_missing;
      }
    } else if (config.wake_pulse_width.count() != 0) {
      return BenchSessionConfigValidationStatus::invalid_wake_pulse;
    }

    if (config.verify_electrical_state &&
        !has_bench_electrical_capability(
            capabilities,
            BenchElectricalCapability::state_feedback)) {
      return BenchSessionConfigValidationStatus::
          electrical_feedback_unsupported;
    }
  }

  if (config.environment_mode ==
          EnvironmentMode::minimal_profile_environment &&
      environment_ == nullptr) {
    return BenchSessionConfigValidationStatus::environment_unavailable;
  }

  if (ecu::core::v2::domain::has_dut_capability(
          dut->capabilities,
          ecu::core::v2::domain::DutCapability::requires_environment) &&
      config.environment_mode !=
          EnvironmentMode::minimal_profile_environment) {
    return BenchSessionConfigValidationStatus::environment_required;
  }

  if (ecu::core::v2::domain::has_dut_capability(
          dut->capabilities,
          ecu::core::v2::domain::DutCapability::requires_power_control) &&
      (!config.use_electrical_control ||
       !config.run_electrical_state.power)) {
    return BenchSessionConfigValidationStatus::power_control_required;
  }

  if (ecu::core::v2::domain::has_dut_capability(
          dut->capabilities,
          ecu::core::v2::domain::DutCapability::requires_wake) &&
      (!config.use_electrical_control ||
       (!config.run_electrical_state.wake &&
        !config.use_wake_pulse))) {
    return BenchSessionConfigValidationStatus::wake_required;
  }

  return BenchSessionConfigValidationStatus::valid;
}

bool BenchSession::configure(
    const BenchSessionConfig& config) noexcept {
  if (busy_) {
    return false;
  }
  ScopedBusy guard{busy_};
  const auto previous_state = state_;

  if (state_ != BenchSessionState::unconfigured &&
      state_ != BenchSessionState::ready) {
    status_ = BenchSessionStatus::invalid_state;
    observe(
        previous_state,
        BenchSessionReason::invalid_state,
        false);
    return false;
  }

  const auto validation = validate_configuration(config);
  if (validation != BenchSessionConfigValidationStatus::valid) {
    status_ =
        validation ==
                BenchSessionConfigValidationStatus::dut_registry_not_frozen
            ? BenchSessionStatus::invalid_state
            : BenchSessionStatus::invalid_argument;
    observe(
        previous_state,
        BenchSessionReason::configuration_rejected,
        false);
    return false;
  }

  const auto* dut = duts_.get(config.dut);
  config_ = config;
  dut_ = dut;

  if (!component_contracts_valid() ||
      !execution_budget_valid(budget_)) {
    config_ = {};
    dut_ = nullptr;
    budget_ = {};
    configured_ = false;
    state_ = BenchSessionState::unconfigured;
    status_ = BenchSessionStatus::execution_contract_invalid;
    fault_source_ = BenchFaultSource::none;
    observe(
        previous_state,
        BenchSessionReason::execution_contract_invalid,
        false);
    return false;
  }

  counters_ = {};
  leases_ = {};
  lease_count_ = 0U;
  operation_token_ = {};
  configured_ = true;
  endpoint_prepared_ = false;
  endpoint_activated_ = false;
  electrical_cleanup_needed_ = false;
  environment_started_ = false;
  cleanup_required_ = false;
  observability_degraded_ = false;
  fault_source_ = BenchFaultSource::none;
  state_ = BenchSessionState::ready;
  status_ = BenchSessionStatus::ok;
  observe(
      previous_state,
      BenchSessionReason::configured,
      true);
  return true;
}

BenchSessionStatus BenchSession::start() noexcept {
  if (busy_) {
    return BenchSessionStatus::busy;
  }
  ScopedBusy guard{busy_};
  const auto previous_state = state_;

  if (!configured_ ||
      state_ != BenchSessionState::ready ||
      dut_ == nullptr) {
    status_ = BenchSessionStatus::invalid_state;
    observe(
        previous_state,
        BenchSessionReason::invalid_state,
        false);
    return status_;
  }

  saturating_increment(counters_.starts);

  const auto cancellation = cancellation_.begin();
  if (cancellation.status !=
      ecu::core::v2::runtime::CancellationBeginStatus::started) {
    status_ = BenchSessionStatus::cancellation_unavailable;
    observe(
        previous_state,
        BenchSessionReason::cancellation_unavailable,
        true);
    return status_;
  }
  operation_token_ = cancellation.token;

  state_ = BenchSessionState::starting;
  fault_source_ = BenchFaultSource::none;
  cleanup_required_ = false;

  const auto acquired = acquire_resources();
  if (acquired != BenchSessionStatus::ok) {
    complete_operation();

    if (acquired == BenchSessionStatus::safe_shutdown_failed) {
      state_ = BenchSessionState::faulted;
      fault_source_ = BenchFaultSource::safe_shutdown;
      status_ = acquired;
      saturating_increment(counters_.faults);
      observe(
          previous_state,
          BenchSessionReason::safe_shutdown_failed,
          true);
      return status_;
    }

    state_ = BenchSessionState::ready;
    fault_source_ = BenchFaultSource::resources;
    status_ = acquired;
    observe(
        previous_state,
        BenchSessionReason::resource_unavailable,
        true);
    return status_;
  }

  if (config_.use_electrical_control) {
    electrical_cleanup_needed_ = true;
    if (!component_ok(electrical_->safe_off())) {
      return fail_start(
          BenchSessionStatus::electrical_fault,
          BenchFaultSource::electrical);
    }
  }

  endpoint_prepared_ = true;
  if (!component_ok(endpoint_.prepare())) {
    return fail_start(
        BenchSessionStatus::dut_fault,
        BenchFaultSource::dut);
  }

  if (config_.environment_mode ==
      EnvironmentMode::minimal_profile_environment) {
    environment_started_ = true;
    if (!component_ok(environment_->start())) {
      return fail_start(
          BenchSessionStatus::environment_fault,
          BenchFaultSource::environment);
    }
  }

  if (config_.use_electrical_control) {
    if (!component_ok(
            electrical_->apply(
                config_.run_electrical_state))) {
      return fail_start(
          BenchSessionStatus::electrical_fault,
          BenchFaultSource::electrical);
    }

    if (config_.use_wake_pulse &&
        !component_ok(
            electrical_->wake_pulse(
                config_.wake_pulse_width))) {
      return fail_start(
          BenchSessionStatus::electrical_fault,
          BenchFaultSource::electrical);
    }

    if (config_.verify_electrical_state) {
      BenchElectricalFeedback feedback{};
      const auto feedback_status =
          electrical_->read_feedback(feedback);
      if (feedback_status != BenchComponentStatus::ok ||
          !verify_electrical_feedback(feedback)) {
        return fail_start(
            BenchSessionStatus::electrical_fault,
            BenchFaultSource::electrical);
      }
    }
  }

  endpoint_activated_ = true;
  if (!component_ok(endpoint_.activate())) {
    return fail_start(
        BenchSessionStatus::dut_fault,
        BenchFaultSource::dut);
  }

  state_ = BenchSessionState::running;
  status_ = BenchSessionStatus::ok;
  fault_source_ = BenchFaultSource::none;
  saturating_increment(counters_.successful_starts);
  observe(
      previous_state,
      BenchSessionReason::started,
      true);
  return status_;
}

BenchSessionStatus BenchSession::service() noexcept {
  if (busy_) {
    return BenchSessionStatus::busy;
  }
  ScopedBusy guard{busy_};
  const auto previous_state = state_;

  if (!configured_ ||
      state_ != BenchSessionState::running) {
    status_ = BenchSessionStatus::invalid_state;
    observe(
        previous_state,
        BenchSessionReason::invalid_state,
        false);
    return status_;
  }

  saturating_increment(counters_.services);

  if (cancellation_.requested(operation_token_)) {
    return cancel_running();
  }

  auto environment_status = BenchComponentStatus::no_action;
  if (environment_started_) {
    environment_status = environment_->service();
    if (environment_status == BenchComponentStatus::fault) {
      return trip_runtime_fault(
          BenchSessionStatus::environment_fault,
          BenchFaultSource::environment);
    }
  }

  const auto endpoint_status = endpoint_.service();
  if (endpoint_status == BenchComponentStatus::fault) {
    return trip_runtime_fault(
        BenchSessionStatus::dut_fault,
        BenchFaultSource::dut);
  }

  const bool endpoint_did_work =
      endpoint_status == BenchComponentStatus::ok;
  const bool environment_did_work =
      environment_started_ &&
      environment_status == BenchComponentStatus::ok;

  status_ = endpoint_did_work || environment_did_work
                ? BenchSessionStatus::ok
                : BenchSessionStatus::no_action;
  observe(previous_state, reason_, false);
  return status_;
}

BenchSessionStatus BenchSession::stop() noexcept {
  if (busy_) {
    return BenchSessionStatus::busy;
  }
  ScopedBusy guard{busy_};
  const auto previous_state = state_;

  if (!configured_) {
    status_ = BenchSessionStatus::invalid_state;
    observe(
        previous_state,
        BenchSessionReason::invalid_state,
        false);
    return status_;
  }
  if (state_ == BenchSessionState::ready) {
    status_ = BenchSessionStatus::no_action;
    observe(previous_state, reason_, false);
    return status_;
  }
  if (state_ != BenchSessionState::running) {
    status_ = BenchSessionStatus::invalid_state;
    observe(
        previous_state,
        BenchSessionReason::invalid_state,
        false);
    return status_;
  }

  saturating_increment(counters_.stops);
  state_ = BenchSessionState::stopping;

  const bool stopped_safely = safe_shutdown();
  complete_operation();

  if (!stopped_safely) {
    state_ = BenchSessionState::faulted;
    fault_source_ = BenchFaultSource::safe_shutdown;
    status_ = BenchSessionStatus::safe_shutdown_failed;
    saturating_increment(counters_.faults);
    observe(
        previous_state,
        BenchSessionReason::safe_shutdown_failed,
        true);
    return status_;
  }

  state_ = BenchSessionState::ready;
  fault_source_ = BenchFaultSource::none;
  status_ = BenchSessionStatus::ok;
  observe(
      previous_state,
      BenchSessionReason::stopped,
      true);
  return status_;
}

BenchSessionStatus BenchSession::recover() noexcept {
  if (busy_) {
    return BenchSessionStatus::busy;
  }
  ScopedBusy guard{busy_};
  const auto previous_state = state_;

  if (!configured_ ||
      state_ != BenchSessionState::faulted) {
    status_ = BenchSessionStatus::invalid_state;
    observe(
        previous_state,
        BenchSessionReason::invalid_state,
        false);
    return status_;
  }

  saturating_increment(counters_.recoveries);
  state_ = BenchSessionState::recovering;
  reason_ = BenchSessionReason::recovery_started;

  const bool recovered = safe_shutdown();
  complete_operation();

  if (!recovered) {
    state_ = BenchSessionState::faulted;
    fault_source_ = BenchFaultSource::safe_shutdown;
    status_ = BenchSessionStatus::safe_shutdown_failed;
    observe(
        previous_state,
        BenchSessionReason::safe_shutdown_failed,
        true);
    return status_;
  }

  state_ = BenchSessionState::ready;
  fault_source_ = BenchFaultSource::none;
  status_ = BenchSessionStatus::ok;
  cleanup_required_ = false;
  observe(
      previous_state,
      BenchSessionReason::recovered,
      true);
  return status_;
}

BenchSessionCancelRequestStatus BenchSession::request_cancel(
    const ecu::core::v2::runtime::CancellationToken token) noexcept {
  if (busy_) {
    return BenchSessionCancelRequestStatus::busy;
  }

  const auto requested = cancellation_.request(token);
  switch (requested) {
    case ecu::core::v2::runtime::CancellationRequestStatus::requested:
      observe(
          state_,
          BenchSessionReason::cancellation_requested,
          false);
      return BenchSessionCancelRequestStatus::requested;
    case ecu::core::v2::runtime::CancellationRequestStatus::already_requested:
      return BenchSessionCancelRequestStatus::already_requested;
    case ecu::core::v2::runtime::CancellationRequestStatus::invalid_token:
      return BenchSessionCancelRequestStatus::invalid_token;
    case ecu::core::v2::runtime::CancellationRequestStatus::stale_token:
      return BenchSessionCancelRequestStatus::stale_token;
    case ecu::core::v2::runtime::CancellationRequestStatus::no_active_operation:
      return BenchSessionCancelRequestStatus::no_active_operation;
  }

  return BenchSessionCancelRequestStatus::no_active_operation;
}

ecu::core::v2::runtime::CancellationToken
BenchSession::cancellation_token() const noexcept {
  return operation_token_;
}

BenchSessionState BenchSession::state() const noexcept {
  return state_;
}

BenchSessionStatus BenchSession::status() const noexcept {
  return status_;
}

BenchFaultSource BenchSession::fault_source() const noexcept {
  return fault_source_;
}

BenchSessionReason BenchSession::reason() const noexcept {
  return reason_;
}

BenchSessionSnapshot BenchSession::snapshot() const noexcept {
  BenchSessionSnapshot result{};
  result.lifecycle_revision = lifecycle_revision_;
  result.state = state_;
  result.status = status_;
  result.reason = reason_;
  result.fault_source = fault_source_;
  result.dut_profile_id =
      dut_ != nullptr ? dut_->profile_id : 0U;
  result.operation_generation = operation_token_.generation;
  result.last_completed_operation_generation =
      last_completed_operation_generation_;
  result.counters = counters_;
  result.active_resource_count = lease_count_;
  result.configured = configured_;
  result.cleanup_required = cleanup_required_;
  result.cancellation_requested =
      cancellation_.requested(operation_token_);
  result.observability_degraded = observability_degraded_;
  return result;
}

BenchSessionCounters BenchSession::counters() const noexcept {
  return counters_;
}

BenchSessionExecutionBudget
BenchSession::execution_budget() const noexcept {
  return budget_;
}

bool BenchSession::cleanup_required() const noexcept {
  return cleanup_required_;
}

const ecu::core::v2::domain::DutDescriptor*
BenchSession::dut_descriptor() const noexcept {
  return dut_;
}

bool BenchSession::component_contracts_valid() const noexcept {
  const auto endpoint = endpoint_.execution_contract();
  if (endpoint.max_prepare_duration.count() <= 0 ||
      endpoint.max_activate_duration.count() <= 0 ||
      endpoint.max_service_duration.count() <= 0 ||
      endpoint.max_safe_stop_duration.count() <= 0 ||
      endpoint.max_stop_duration.count() <= 0) {
    return false;
  }

  if (config_.environment_mode ==
      EnvironmentMode::minimal_profile_environment) {
    const auto environment = environment_->execution_contract();
    if (environment.max_start_duration.count() <= 0 ||
        environment.max_service_duration.count() <= 0 ||
        environment.max_stop_duration.count() <= 0) {
      return false;
    }
  }

  if (config_.use_electrical_control) {
    const auto electrical = electrical_->execution_contract();
    if (electrical.max_apply_duration.count() <= 0 ||
        electrical.max_safe_off_duration.count() <= 0) {
      return false;
    }

    if (config_.use_wake_pulse &&
        electrical.max_wake_pulse_command_duration.count() <= 0) {
      return false;
    }

    if (config_.verify_electrical_state &&
        electrical.max_feedback_duration.count() <= 0) {
      return false;
    }
  }

  if (events_ != nullptr) {
    const auto events = events_->execution_contract();
    if (!events_->ready() ||
        events.max_publish_duration.count() <= 0) {
      return false;
    }
  }

  return true;
}

bool BenchSession::execution_budget_valid(
    BenchSessionExecutionBudget& budget) const noexcept {
  budget = {};

  const auto endpoint = endpoint_.execution_contract();
  ecu::core::v2::time::MonotonicDuration cleanup{0};

  if (!add_duration(
          cleanup,
          endpoint.max_safe_stop_duration,
          cleanup) ||
      !add_duration(
          cleanup,
          endpoint.max_stop_duration,
          cleanup)) {
    return false;
  }

  if (config_.environment_mode ==
      EnvironmentMode::minimal_profile_environment) {
    const auto environment = environment_->execution_contract();
    if (!add_duration(
            cleanup,
            environment.max_stop_duration,
            cleanup)) {
      return false;
    }
  }

  if (config_.use_electrical_control) {
    const auto electrical = electrical_->execution_contract();
    if (!add_duration(
            cleanup,
            electrical.max_safe_off_duration,
            cleanup)) {
      return false;
    }
  }

  ecu::core::v2::time::MonotonicDuration start{0};
  ecu::core::v2::time::MonotonicDuration service{
      endpoint.max_service_duration};

  if (config_.use_electrical_control) {
    const auto electrical = electrical_->execution_contract();
    if (!add_duration(
            start,
            electrical.max_safe_off_duration,
            start)) {
      return false;
    }
  }

  if (!add_duration(
          start,
          endpoint.max_prepare_duration,
          start)) {
    return false;
  }

  if (config_.environment_mode ==
      EnvironmentMode::minimal_profile_environment) {
    const auto environment = environment_->execution_contract();
    if (!add_duration(
            start,
            environment.max_start_duration,
            start) ||
        !add_duration(
            service,
            environment.max_service_duration,
            service)) {
      return false;
    }
  }

  if (config_.use_electrical_control) {
    const auto electrical = electrical_->execution_contract();
    if (!add_duration(
            start,
            electrical.max_apply_duration,
            start)) {
      return false;
    }

    if (config_.use_wake_pulse &&
        !add_duration(
            start,
            electrical.max_wake_pulse_command_duration,
            start)) {
      return false;
    }

    if (config_.verify_electrical_state &&
        !add_duration(
            start,
            electrical.max_feedback_duration,
            start)) {
      return false;
    }
  }

  if (!add_duration(
          start,
          endpoint.max_activate_duration,
          start) ||
      !add_duration(start, cleanup, start) ||
      !add_duration(service, cleanup, service)) {
    return false;
  }

  auto stop = cleanup;
  auto recover = cleanup;

  if (events_ != nullptr) {
    const auto events = events_->execution_contract();
    if (!add_duration(
            start,
            events.max_publish_duration,
            start) ||
        !add_duration(
            service,
            events.max_publish_duration,
            service) ||
        !add_duration(
            stop,
            events.max_publish_duration,
            stop) ||
        !add_duration(
            recover,
            events.max_publish_duration,
            recover)) {
      return false;
    }
  }

  budget.valid = true;
  budget.max_start_duration = start;
  budget.max_service_duration = service;
  budget.max_stop_duration = stop;
  budget.max_recover_duration = recover;
  return true;
}

bool BenchSession::add_duration(
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

BenchSessionStatus BenchSession::acquire_resources() noexcept {
  leases_ = {};
  lease_count_ = 0U;

  const ecu::core::v2::runtime::ResourceKey dut_resource{
      ecu::core::v2::runtime::ResourceClass::device_under_test,
      dut_->profile_id};

  const auto dut_lease =
      resources_.acquire(
          dut_resource,
          config_.session_owner);
  if (dut_lease.status !=
      ecu::core::v2::runtime::ResourceAcquireStatus::acquired) {
    saturating_increment(counters_.resource_contentions);
    return BenchSessionStatus::resource_unavailable;
  }

  leases_[lease_count_] = dut_lease.lease;
  ++lease_count_;

  for (std::size_t index = 0U;
       index <
           static_cast<std::size_t>(
               config_.additional_resource_count);
       ++index) {
    const auto acquired =
        resources_.acquire(
            config_.additional_resources[index],
            config_.session_owner);
    if (acquired.status !=
        ecu::core::v2::runtime::ResourceAcquireStatus::acquired) {
      saturating_increment(counters_.resource_contentions);
      if (!release_resources()) {
        cleanup_required_ = false;
        return BenchSessionStatus::safe_shutdown_failed;
      }
      return BenchSessionStatus::resource_unavailable;
    }

    leases_[lease_count_] = acquired.lease;
    ++lease_count_;
  }

  return BenchSessionStatus::ok;
}

bool BenchSession::release_resources() noexcept {
  bool success = true;

  while (lease_count_ > 0U) {
    --lease_count_;
    const auto released =
        resources_.release(leases_[lease_count_]);
    if (released !=
        ecu::core::v2::runtime::ResourceReleaseStatus::released) {
      success = false;
    }
    leases_[lease_count_] = {};
  }

  return success;
}

BenchSessionStatus BenchSession::fail_start(
    const BenchSessionStatus failure_status,
    const BenchFaultSource source) noexcept {
  saturating_increment(counters_.faults);
  fault_source_ = source;

  const bool stopped_safely = safe_shutdown();
  complete_operation();

  state_ = BenchSessionState::faulted;
  if (!stopped_safely) {
    fault_source_ = BenchFaultSource::safe_shutdown;
    status_ = BenchSessionStatus::safe_shutdown_failed;
    observe(
        BenchSessionState::ready,
        BenchSessionReason::safe_shutdown_failed,
        true);
    return status_;
  }

  status_ = failure_status;
  observe(
      BenchSessionState::ready,
      failure_reason(failure_status, source),
      true);
  return status_;
}

BenchSessionStatus BenchSession::trip_runtime_fault(
    const BenchSessionStatus failure_status,
    const BenchFaultSource source) noexcept {
  saturating_increment(counters_.faults);
  fault_source_ = source;
  state_ = BenchSessionState::stopping;

  const bool stopped_safely = safe_shutdown();
  complete_operation();

  state_ = BenchSessionState::faulted;
  if (!stopped_safely) {
    fault_source_ = BenchFaultSource::safe_shutdown;
    status_ = BenchSessionStatus::safe_shutdown_failed;
    observe(
        BenchSessionState::running,
        BenchSessionReason::safe_shutdown_failed,
        true);
    return status_;
  }

  status_ = failure_status;
  observe(
      BenchSessionState::running,
      failure_reason(failure_status, source),
      true);
  return status_;
}

BenchSessionStatus BenchSession::cancel_running() noexcept {
  saturating_increment(counters_.cancellations);
  state_ = BenchSessionState::stopping;

  const bool stopped_safely = safe_shutdown();
  complete_operation();

  if (!stopped_safely) {
    saturating_increment(counters_.faults);
    state_ = BenchSessionState::faulted;
    fault_source_ = BenchFaultSource::safe_shutdown;
    status_ = BenchSessionStatus::safe_shutdown_failed;
    observe(
        BenchSessionState::running,
        BenchSessionReason::safe_shutdown_failed,
        true);
    return status_;
  }

  state_ = BenchSessionState::ready;
  fault_source_ = BenchFaultSource::none;
  status_ = BenchSessionStatus::cancelled;
  observe(
      BenchSessionState::running,
      BenchSessionReason::cancelled,
      true);
  return status_;
}

bool BenchSession::safe_shutdown() noexcept {
  saturating_increment(counters_.safe_shutdowns);
  bool success = true;

  if (endpoint_activated_) {
    if (component_ok(endpoint_.safe_stop())) {
      endpoint_activated_ = false;
    } else {
      success = false;
    }
  }

  if (electrical_cleanup_needed_) {
    if (component_ok(electrical_->safe_off())) {
      electrical_cleanup_needed_ = false;
    } else {
      success = false;
    }
  }

  if (environment_started_) {
    if (component_ok(environment_->stop())) {
      environment_started_ = false;
    } else {
      success = false;
    }
  }

  if (endpoint_prepared_) {
    if (component_ok(endpoint_.stop())) {
      endpoint_prepared_ = false;
      endpoint_activated_ = false;
    } else {
      success = false;
    }
  }

  if (!release_resources()) {
    success = false;
  }

  cleanup_required_ =
      endpoint_prepared_ ||
      endpoint_activated_ ||
      electrical_cleanup_needed_ ||
      environment_started_ ||
      lease_count_ != 0U;

  if (!success) {
    saturating_increment(counters_.safe_shutdown_failures);
  }

  return success;
}

void BenchSession::complete_operation() noexcept {
  if (operation_token_.valid()) {
    last_completed_operation_generation_ =
        operation_token_.generation;
    (void)cancellation_.complete(operation_token_);
    operation_token_ = {};
  }
}

bool BenchSession::verify_electrical_feedback(
    const BenchElectricalFeedback& feedback) const noexcept {
  if (!feedback.state_valid ||
      feedback.state.power !=
          config_.run_electrical_state.power ||
      feedback.state.ignition !=
          config_.run_electrical_state.ignition) {
    return false;
  }

  if (config_.run_electrical_state.wake) {
    return feedback.state.wake;
  }

  if (!config_.use_wake_pulse) {
    return !feedback.state.wake;
  }

  return true;
}

void BenchSession::observe(
    const BenchSessionState previous_state,
    const BenchSessionReason reason,
    const bool publish_event) noexcept {
  lifecycle_revision_ =
      ecu::core::v2::runtime::next_state_revision(
          lifecycle_revision_);
  reason_ = reason;

  if (!publish_event || events_ == nullptr) {
    return;
  }

  BenchSessionEvent event{};
  event.kind = state_ == BenchSessionState::faulted
                   ? BenchSessionEventKind::fault
                   : previous_state != state_
                         ? BenchSessionEventKind::state_transition
                         : BenchSessionEventKind::operation;
  event.previous_state = previous_state;
  event.snapshot = snapshot();

  const auto published = events_->publish(event);
  if (published == BenchSessionEventPublishStatus::published ||
      published == BenchSessionEventPublishStatus::no_subscribers) {
    return;
  }

  saturating_increment(counters_.event_publish_failures);
  observability_degraded_ = true;
  lifecycle_revision_ =
      ecu::core::v2::runtime::next_state_revision(
          lifecycle_revision_);
}

BenchSessionReason BenchSession::failure_reason(
    const BenchSessionStatus status,
    const BenchFaultSource source) const noexcept {
  if (status == BenchSessionStatus::safe_shutdown_failed ||
      source == BenchFaultSource::safe_shutdown) {
    return BenchSessionReason::safe_shutdown_failed;
  }

  switch (source) {
    case BenchFaultSource::resources:
      return BenchSessionReason::resource_unavailable;
    case BenchFaultSource::electrical:
      return BenchSessionReason::electrical_fault;
    case BenchFaultSource::environment:
      return BenchSessionReason::environment_fault;
    case BenchFaultSource::dut:
      return BenchSessionReason::dut_fault;
    case BenchFaultSource::safe_shutdown:
      return BenchSessionReason::safe_shutdown_failed;
    case BenchFaultSource::none:
      break;
  }

  switch (status) {
    case BenchSessionStatus::resource_unavailable:
      return BenchSessionReason::resource_unavailable;
    case BenchSessionStatus::electrical_fault:
      return BenchSessionReason::electrical_fault;
    case BenchSessionStatus::environment_fault:
      return BenchSessionReason::environment_fault;
    case BenchSessionStatus::dut_fault:
      return BenchSessionReason::dut_fault;
    case BenchSessionStatus::safe_shutdown_failed:
      return BenchSessionReason::safe_shutdown_failed;
    default:
      return BenchSessionReason::none;
  }
}

}  // namespace ecu::bench
