#include "ecu/dut_profile/runtime.hpp"

#include <limits>

namespace ecu::dut_profile {

namespace {

void saturating_increment(std::uint32_t& value) noexcept {
  if (value != (std::numeric_limits<std::uint32_t>::max)()) {
    ++value;
  }
}

[[nodiscard]] constexpr bool component_ok(
    const ecu::bench::BenchComponentStatus status) noexcept {
  return status == ecu::bench::BenchComponentStatus::ok ||
         status == ecu::bench::BenchComponentStatus::no_action;
}

}  // namespace

DutProfileSessionEndpoint::DutProfileSessionEndpoint(
    const ResolvedDutSessionPlan& plan,
    IDutProfileProgram& program) noexcept
    : plan_(plan),
      program_(program),
      contract_(program.execution_contract()),
      valid_(descriptor_matches() && execution_contract_valid()) {}

ecu::bench::BenchComponentExecutionContract
DutProfileSessionEndpoint::execution_contract() const noexcept {
  return valid_
             ? contract_
             : ecu::bench::BenchComponentExecutionContract{};
}

ecu::bench::BenchComponentStatus
DutProfileSessionEndpoint::prepare() noexcept {
  saturating_increment(counters_.prepare_calls);

  if (!valid_) {
    return reject();
  }
  if (state_ == DutProfileEndpointState::prepared) {
    last_status_ = ecu::bench::BenchComponentStatus::no_action;
    return last_status_;
  }
  if (state_ != DutProfileEndpointState::idle) {
    return reject();
  }

  return accept_result(
      program_.prepare(plan_),
      DutProfileEndpointState::prepared);
}

ecu::bench::BenchComponentStatus
DutProfileSessionEndpoint::activate() noexcept {
  saturating_increment(counters_.activate_calls);

  if (!valid_) {
    return reject();
  }
  if (state_ == DutProfileEndpointState::active) {
    last_status_ = ecu::bench::BenchComponentStatus::no_action;
    return last_status_;
  }
  if (state_ != DutProfileEndpointState::prepared) {
    return reject();
  }

  return accept_result(
      program_.activate(plan_),
      DutProfileEndpointState::active);
}

ecu::bench::BenchComponentStatus
DutProfileSessionEndpoint::service() noexcept {
  saturating_increment(counters_.service_calls);

  if (!valid_ || state_ != DutProfileEndpointState::active) {
    return reject();
  }

  return accept_result(
      program_.service(plan_),
      DutProfileEndpointState::active);
}

ecu::bench::BenchComponentStatus
DutProfileSessionEndpoint::safe_stop() noexcept {
  saturating_increment(counters_.safe_stop_calls);

  if (!valid_) {
    return reject();
  }

  if (state_ == DutProfileEndpointState::idle ||
      state_ == DutProfileEndpointState::prepared ||
      state_ == DutProfileEndpointState::safe_stopped) {
    last_status_ = ecu::bench::BenchComponentStatus::no_action;
    return last_status_;
  }

  if (state_ != DutProfileEndpointState::active &&
      state_ != DutProfileEndpointState::faulted) {
    return reject();
  }

  return accept_result(
      program_.safe_stop(plan_),
      DutProfileEndpointState::safe_stopped);
}

ecu::bench::BenchComponentStatus
DutProfileSessionEndpoint::stop() noexcept {
  saturating_increment(counters_.stop_calls);

  if (!valid_) {
    return reject();
  }
  if (state_ == DutProfileEndpointState::idle) {
    last_status_ = ecu::bench::BenchComponentStatus::no_action;
    return last_status_;
  }
  if (state_ == DutProfileEndpointState::active) {
    return reject();
  }
  if (state_ != DutProfileEndpointState::prepared &&
      state_ != DutProfileEndpointState::safe_stopped &&
      state_ != DutProfileEndpointState::faulted) {
    return reject();
  }

  return accept_result(
      program_.stop(plan_),
      DutProfileEndpointState::idle);
}

bool DutProfileSessionEndpoint::valid() const noexcept {
  return valid_;
}

DutProfileEndpointSnapshot
DutProfileSessionEndpoint::snapshot() const noexcept {
  DutProfileEndpointSnapshot result{};
  result.valid = valid_;
  result.state = state_;
  result.last_status = last_status_;
  result.profile_id = plan_.profile_id;
  result.profile_revision = plan_.profile_revision;
  result.counters = counters_;
  return result;
}

bool DutProfileSessionEndpoint::descriptor_matches() const noexcept {
  const auto descriptor = program_.descriptor();
  return is_valid_program_descriptor(descriptor) &&
         plan_.schema_version == descriptor.schema_version &&
         plan_.profile_revision == descriptor.profile_revision &&
         plan_.profile_id == descriptor.profile_id;
}

bool DutProfileSessionEndpoint::execution_contract_valid() const noexcept {
  return contract_.max_prepare_duration.count() > 0 &&
         contract_.max_activate_duration.count() > 0 &&
         contract_.max_service_duration.count() > 0 &&
         contract_.max_safe_stop_duration.count() > 0 &&
         contract_.max_stop_duration.count() > 0;
}

ecu::bench::BenchComponentStatus
DutProfileSessionEndpoint::reject() noexcept {
  note_fault();
  last_status_ = ecu::bench::BenchComponentStatus::fault;
  return last_status_;
}

ecu::bench::BenchComponentStatus
DutProfileSessionEndpoint::accept_result(
    const ecu::bench::BenchComponentStatus status,
    const DutProfileEndpointState success_state) noexcept {
  last_status_ = status;
  if (!component_ok(status)) {
    note_fault();
    return last_status_;
  }

  state_ = success_state;
  return last_status_;
}

void DutProfileSessionEndpoint::note_fault() noexcept {
  state_ = DutProfileEndpointState::faulted;
  saturating_increment(counters_.faults);
}

}  // namespace ecu::dut_profile
