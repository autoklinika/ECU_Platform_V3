#include "ecu/bench/session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using ecu::bench::BenchComponentExecutionContract;
using ecu::bench::BenchComponentStatus;
using ecu::bench::BenchElectricalCapability;
using ecu::bench::BenchElectricalCapabilityMask;
using ecu::bench::BenchElectricalExecutionContract;
using ecu::bench::BenchElectricalFeedback;
using ecu::bench::BenchElectricalState;
using ecu::bench::BenchEnvironmentExecutionContract;
using ecu::bench::BenchSession;
using ecu::bench::BenchSessionConfig;
using ecu::bench::BenchSessionState;
using ecu::bench::BenchSessionStatus;
using ecu::bench::EnvironmentMode;
namespace domain = ecu::core::v2::domain;
namespace runtime = ecu::core::v2::runtime;
namespace time = ecu::core::v2::time;

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

enum class Call : std::uint8_t {
  endpoint_prepare,
  endpoint_activate,
  endpoint_service,
  endpoint_safe_stop,
  endpoint_stop,
  environment_start,
  environment_service,
  environment_stop,
  electrical_apply,
  electrical_pulse,
  electrical_feedback,
  electrical_safe_off,
};

class CallLog final {
 public:
  static constexpr std::size_t kCapacity = 64U;

  void push(const Call call) noexcept {
    if (count_ < calls_.size()) {
      calls_[count_] = call;
      ++count_;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return count_;
  }

  [[nodiscard]] Call at(const std::size_t index) const noexcept {
    return calls_[index];
  }

 private:
  std::array<Call, kCapacity> calls_{};
  std::size_t count_{0U};
};

class FakeEndpoint final : public ecu::bench::IDutSessionEndpoint {
 public:
  explicit FakeEndpoint(CallLog& log) noexcept : log_(log) {}

  [[nodiscard]] BenchComponentExecutionContract
  execution_contract() const noexcept override {
    return contract;
  }

  [[nodiscard]] BenchComponentStatus prepare() noexcept override {
    log_.push(Call::endpoint_prepare);
    return prepare_status;
  }

  [[nodiscard]] BenchComponentStatus activate() noexcept override {
    log_.push(Call::endpoint_activate);
    return activate_status;
  }

  [[nodiscard]] BenchComponentStatus service() noexcept override {
    log_.push(Call::endpoint_service);
    return service_status;
  }

  [[nodiscard]] BenchComponentStatus safe_stop() noexcept override {
    log_.push(Call::endpoint_safe_stop);
    return safe_stop_status;
  }

  [[nodiscard]] BenchComponentStatus stop() noexcept override {
    log_.push(Call::endpoint_stop);
    return stop_status;
  }

  BenchComponentExecutionContract contract{
      time::MonotonicDuration{10},
      time::MonotonicDuration{20},
      time::MonotonicDuration{30},
      time::MonotonicDuration{40},
      time::MonotonicDuration{50}};
  BenchComponentStatus prepare_status{BenchComponentStatus::ok};
  BenchComponentStatus activate_status{BenchComponentStatus::ok};
  BenchComponentStatus service_status{BenchComponentStatus::ok};
  BenchComponentStatus safe_stop_status{BenchComponentStatus::ok};
  BenchComponentStatus stop_status{BenchComponentStatus::ok};

 private:
  CallLog& log_;
};

class FakeEnvironment final : public ecu::bench::IEnvironmentSession {
 public:
  explicit FakeEnvironment(CallLog& log) noexcept : log_(log) {}

  [[nodiscard]] BenchEnvironmentExecutionContract
  execution_contract() const noexcept override {
    return contract;
  }

  [[nodiscard]] BenchComponentStatus start() noexcept override {
    log_.push(Call::environment_start);
    return start_status;
  }

  [[nodiscard]] BenchComponentStatus service() noexcept override {
    log_.push(Call::environment_service);
    return service_status;
  }

  [[nodiscard]] BenchComponentStatus stop() noexcept override {
    log_.push(Call::environment_stop);
    return stop_status;
  }

  BenchEnvironmentExecutionContract contract{
      time::MonotonicDuration{60},
      time::MonotonicDuration{70},
      time::MonotonicDuration{80}};
  BenchComponentStatus start_status{BenchComponentStatus::ok};
  BenchComponentStatus service_status{BenchComponentStatus::ok};
  BenchComponentStatus stop_status{BenchComponentStatus::ok};

 private:
  CallLog& log_;
};

class FakeElectrical final : public ecu::bench::IBenchElectricalControl {
 public:
  explicit FakeElectrical(CallLog& log) noexcept : log_(log) {}

  [[nodiscard]] BenchElectricalCapabilityMask
  capabilities() const noexcept override {
    return capability_mask;
  }

  [[nodiscard]] BenchElectricalExecutionContract
  execution_contract() const noexcept override {
    return contract;
  }

  [[nodiscard]] BenchComponentStatus apply(
      const BenchElectricalState state) noexcept override {
    log_.push(Call::electrical_apply);
    last_applied = state;
    return apply_status;
  }

  [[nodiscard]] BenchComponentStatus wake_pulse(
      const time::MonotonicDuration pulse_width) noexcept override {
    log_.push(Call::electrical_pulse);
    last_pulse_width = pulse_width;
    return pulse_status;
  }

  [[nodiscard]] BenchComponentStatus read_feedback(
      BenchElectricalFeedback& destination) noexcept override {
    log_.push(Call::electrical_feedback);
    destination = feedback;
    return feedback_status;
  }

  [[nodiscard]] BenchComponentStatus safe_off() noexcept override {
    log_.push(Call::electrical_safe_off);
    return safe_off_status;
  }

  BenchElectricalCapabilityMask capability_mask{
      ecu::bench::bench_electrical_capability_mask(
          BenchElectricalCapability::power) |
      ecu::bench::bench_electrical_capability_mask(
          BenchElectricalCapability::ignition) |
      ecu::bench::bench_electrical_capability_mask(
          BenchElectricalCapability::wake_level) |
      ecu::bench::bench_electrical_capability_mask(
          BenchElectricalCapability::wake_pulse) |
      ecu::bench::bench_electrical_capability_mask(
          BenchElectricalCapability::state_feedback) |
      ecu::bench::bench_electrical_capability_mask(
          BenchElectricalCapability::voltage_feedback) |
      ecu::bench::bench_electrical_capability_mask(
          BenchElectricalCapability::current_feedback)};

  BenchElectricalExecutionContract contract{
      time::MonotonicDuration{90},
      time::MonotonicDuration{100},
      time::MonotonicDuration{120},
      time::MonotonicDuration{110}};
  BenchElectricalFeedback feedback{
      true,
      {true, true, false},
      true,
      24000,
      true,
      1500};
  BenchElectricalState last_applied{};
  time::MonotonicDuration last_pulse_width{0};
  BenchComponentStatus apply_status{BenchComponentStatus::ok};
  BenchComponentStatus pulse_status{BenchComponentStatus::ok};
  BenchComponentStatus feedback_status{BenchComponentStatus::ok};
  BenchComponentStatus safe_off_status{BenchComponentStatus::ok};

 private:
  CallLog& log_;
};

[[nodiscard]] domain::DutCapabilityMask capability(
    const domain::DutCapability value) noexcept {
  return domain::dut_capability_mask(value);
}

[[nodiscard]] domain::DutDescriptor make_dut(
    const domain::DutProfileId profile_id,
    const domain::DutCapabilityMask capabilities) noexcept {
  return {
      profile_id,
      domain::DutClass::actuator,
      domain::kTruck,
      capabilities};
}

[[nodiscard]] BenchSessionConfig base_config(
    const runtime::DutHandle dut,
    const runtime::ResourceOwnerId owner) noexcept {
  BenchSessionConfig config{};
  config.session_owner = owner;
  config.dut = dut;
  return config;
}

}  // namespace

int main() {
  int failures = 0;

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registration = duts.register_dut(
        make_dut(
            0x2001U,
            capability(domain::DutCapability::raw_can) |
                capability(domain::DutCapability::requires_environment) |
                capability(domain::DutCapability::requires_power_control) |
                capability(domain::DutCapability::requires_wake)));
    failures += require(
        registration.status ==
            runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "happy-path DUT registry freezes");

    CallLog log;
    FakeEndpoint endpoint{log};
    FakeEnvironment environment{log};
    FakeElectrical electrical{log};
    BenchSession session{
        resources,
        duts,
        endpoint,
        &electrical,
        &environment};

    auto config = base_config(registration.handle, 101U);
    config.additional_resources[0U] = {
        runtime::ResourceClass::can_channel,
        1U};
    config.additional_resource_count = 1U;
    config.use_electrical_control = true;
    config.run_electrical_state = {true, true, false};
    config.use_wake_pulse = true;
    config.wake_pulse_width = time::MonotonicDuration{25};
    config.verify_electrical_state = true;
    config.environment_mode =
        EnvironmentMode::minimal_profile_environment;

    failures += require(
        session.configure(config) &&
            session.state() == BenchSessionState::ready &&
            session.dut_descriptor() != nullptr &&
            session.dut_descriptor()->profile_id == 0x2001U,
        "session configures against frozen DUT topology");

    const auto budget = session.execution_budget();
    failures += require(
        budget.valid &&
            budget.max_start_duration.count() == 790 &&
            budget.max_service_duration.count() == 380 &&
            budget.max_stop_duration.count() == 280 &&
            budget.max_recover_duration.count() == 280,
        "session publishes conservative bounded execution budget");

    failures += require(
        session.start() == BenchSessionStatus::ok &&
            session.state() == BenchSessionState::running &&
            resources.active_count() == 2U &&
            electrical.last_applied ==
                BenchElectricalState{true, true, false} &&
            electrical.last_pulse_width ==
                time::MonotonicDuration{25},
        "session starts DUT-neutral bench lifecycle and owns resources");

    failures += require(
        log.size() == 7U &&
            log.at(0U) == Call::electrical_safe_off &&
            log.at(1U) == Call::endpoint_prepare &&
            log.at(2U) == Call::environment_start &&
            log.at(3U) == Call::electrical_apply &&
            log.at(4U) == Call::electrical_pulse &&
            log.at(5U) == Call::electrical_feedback &&
            log.at(6U) == Call::endpoint_activate,
        "start ordering prepares transport before environment and power activation");

    endpoint.service_status = BenchComponentStatus::no_action;
    environment.service_status = BenchComponentStatus::ok;
    failures += require(
        session.service() == BenchSessionStatus::ok,
        "environment work prevents false no-action result");

    environment.service_status = BenchComponentStatus::no_action;
    failures += require(
        session.service() == BenchSessionStatus::no_action,
        "service reports no-action only when all active components do no work");

    failures += require(
        session.stop() == BenchSessionStatus::ok &&
            session.state() == BenchSessionState::ready &&
            resources.active_count() == 0U &&
            !session.cleanup_required(),
        "normal stop reaches safe ready state and releases all resources");

    failures += require(
        log.size() == 15U &&
            log.at(11U) == Call::endpoint_safe_stop &&
            log.at(12U) == Call::electrical_safe_off &&
            log.at(13U) == Call::environment_stop &&
            log.at(14U) == Call::endpoint_stop,
        "stop ordering neutralizes DUT before power-off and transport teardown");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registration = duts.register_dut(
        make_dut(
            0x2002U,
            capability(domain::DutCapability::raw_can)));
    failures += require(
        registration.status ==
            runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "resource-contention DUT registry freezes");

    CallLog log;
    FakeEndpoint endpoint{log};
    BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr};

    const runtime::ResourceKey can0{
        runtime::ResourceClass::can_channel,
        7U};
    const auto external = resources.acquire(can0, 900U);
    failures += require(
        external.status ==
            runtime::ResourceAcquireStatus::acquired,
        "contention fixture owns CAN resource");

    auto config = base_config(registration.handle, 102U);
    config.additional_resources[0U] = can0;
    config.additional_resource_count = 1U;

    failures += require(
        session.configure(config) &&
            session.start() ==
                BenchSessionStatus::resource_unavailable &&
            session.state() == BenchSessionState::ready &&
            resources.active_count() == 1U &&
            log.size() == 0U &&
            session.counters().resource_contentions == 1U,
        "resource contention is fail-closed without leaking DUT lease");

    failures += require(
        resources.release(external.lease) ==
            runtime::ResourceReleaseStatus::released,
        "contention fixture releases external lease");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registration = duts.register_dut(
        make_dut(
            0x2003U,
            capability(domain::DutCapability::raw_can)));
    failures += require(
        registration.status ==
            runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "activation-failure DUT registry freezes");

    CallLog log;
    FakeEndpoint endpoint{log};
    endpoint.activate_status = BenchComponentStatus::fault;

    BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr};
    const auto config = base_config(registration.handle, 103U);

    failures += require(
        session.configure(config) &&
            session.start() == BenchSessionStatus::dut_fault &&
            session.state() == BenchSessionState::faulted &&
            resources.active_count() == 0U &&
            !session.cleanup_required(),
        "activation fault performs bounded cleanup and latches fault");

    failures += require(
        log.size() == 4U &&
            log.at(0U) == Call::endpoint_prepare &&
            log.at(1U) == Call::endpoint_activate &&
            log.at(2U) == Call::endpoint_safe_stop &&
            log.at(3U) == Call::endpoint_stop,
        "failed activation is treated as potentially active and safely stopped");

    endpoint.activate_status = BenchComponentStatus::ok;
    failures += require(
        session.recover() == BenchSessionStatus::ok &&
            session.state() == BenchSessionState::ready,
        "explicit recovery acknowledges cleaned fault before restart");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registration = duts.register_dut(
        make_dut(
            0x2004U,
            capability(domain::DutCapability::raw_can)));
    failures += require(
        registration.status ==
            runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "cleanup-failure DUT registry freezes");

    CallLog log;
    FakeEndpoint endpoint{log};
    BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr};
    const auto config = base_config(registration.handle, 104U);

    failures += require(
        session.configure(config) &&
            session.start() == BenchSessionStatus::ok,
        "cleanup-failure fixture starts");

    endpoint.stop_status = BenchComponentStatus::fault;
    failures += require(
        session.stop() ==
                BenchSessionStatus::safe_shutdown_failed &&
            session.state() == BenchSessionState::faulted &&
            session.cleanup_required() &&
            resources.active_count() == 0U,
        "failed component teardown remains explicitly recoverable");

    endpoint.stop_status = BenchComponentStatus::ok;
    failures += require(
        session.recover() == BenchSessionStatus::ok &&
            session.state() == BenchSessionState::ready &&
            !session.cleanup_required(),
        "recovery retries incomplete cleanup before returning ready");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registration = duts.register_dut(
        make_dut(
            0x2005U,
            capability(domain::DutCapability::raw_can)));
    failures += require(
        registration.status ==
            runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "cancellation DUT registry freezes");

    CallLog log;
    FakeEndpoint endpoint{log};
    BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr};
    const auto config = base_config(registration.handle, 105U);

    failures += require(
        session.configure(config) &&
            session.start() == BenchSessionStatus::ok,
        "cancellation fixture starts first generation");

    const auto first = session.cancellation_token();
    failures += require(
        first.valid() &&
            session.request_cancel(first) ==
                ecu::bench::BenchSessionCancelRequestStatus::requested &&
            session.service() == BenchSessionStatus::cancelled &&
            session.state() == BenchSessionState::ready &&
            resources.active_count() == 0U,
        "generation-scoped cancellation safely terminates running session");

    failures += require(
        session.start() == BenchSessionStatus::ok,
        "cancellation fixture starts second generation");
    const auto second = session.cancellation_token();

    failures += require(
        second.valid() &&
            second.generation != first.generation &&
            session.request_cancel(first) ==
                ecu::bench::BenchSessionCancelRequestStatus::stale_token &&
            session.stop() == BenchSessionStatus::ok,
        "stale cancellation token cannot stop a later session generation");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registration = duts.register_dut(
        make_dut(
            0x2006U,
            capability(domain::DutCapability::raw_can) |
                capability(domain::DutCapability::requires_wake)));
    failures += require(
        registration.status ==
            runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "electrical-capability DUT registry freezes");

    CallLog log;
    FakeEndpoint endpoint{log};
    FakeElectrical electrical{log};
    electrical.capability_mask =
        ecu::bench::bench_electrical_capability_mask(
            BenchElectricalCapability::power);

    BenchSession session{
        resources,
        duts,
        endpoint,
        &electrical,
        nullptr};
    auto config = base_config(registration.handle, 106U);
    config.use_electrical_control = true;
    config.run_electrical_state = {true, false, false};
    config.use_wake_pulse = true;
    config.wake_pulse_width = time::MonotonicDuration{10};

    failures += require(
        !session.configure(config) &&
            session.status() ==
                BenchSessionStatus::invalid_argument,
        "missing wake capability rejects incompatible bench configuration");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registration = duts.register_dut(
        make_dut(
            0x2007U,
            capability(domain::DutCapability::raw_can)));

    CallLog log;
    FakeEndpoint endpoint{log};
    BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr};
    const auto config = base_config(registration.handle, 107U);

    failures += require(
        !session.configure(config) &&
            session.status() ==
                BenchSessionStatus::invalid_state,
        "bench session refuses mutable DUT registry topology");

    failures += require(
        duts.freeze_configuration(),
        "mutable-topology fixture freezes after rejection");
    endpoint.contract.max_service_duration =
        time::MonotonicDuration{0};

    failures += require(
        !session.configure(config) &&
            session.status() ==
                BenchSessionStatus::execution_contract_invalid,
        "zero execution bound fails closed during configuration");
  }

  if (failures == 0) {
    std::cout << "BENCH_RUNTIME_SESSION_V1=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
