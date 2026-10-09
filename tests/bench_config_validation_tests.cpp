#include "ecu/bench/session.hpp"

#include <cstdint>
#include <iostream>

namespace {

namespace bench = ecu::bench;
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

class Endpoint final : public bench::IDutSessionEndpoint {
 public:
  [[nodiscard]] bench::BenchComponentExecutionContract
  execution_contract() const noexcept override {
    return {
        time::MonotonicDuration{10},
        time::MonotonicDuration{10},
        time::MonotonicDuration{10},
        time::MonotonicDuration{10},
        time::MonotonicDuration{10}};
  }

  [[nodiscard]] bench::BenchComponentStatus prepare() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus activate() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus service() noexcept override {
    return bench::BenchComponentStatus::no_action;
  }
  [[nodiscard]] bench::BenchComponentStatus safe_stop() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus stop() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
};

class Environment final : public bench::IEnvironmentSession {
 public:
  [[nodiscard]] bench::BenchEnvironmentExecutionContract
  execution_contract() const noexcept override {
    return {
        time::MonotonicDuration{10},
        time::MonotonicDuration{10},
        time::MonotonicDuration{10}};
  }

  [[nodiscard]] bench::BenchComponentStatus start() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus service() noexcept override {
    return bench::BenchComponentStatus::no_action;
  }
  [[nodiscard]] bench::BenchComponentStatus stop() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
};

class Electrical final : public bench::IBenchElectricalControl {
 public:
  [[nodiscard]] bench::BenchElectricalCapabilityMask
  capabilities() const noexcept override {
    return capability_mask;
  }

  [[nodiscard]] bench::BenchElectricalExecutionContract
  execution_contract() const noexcept override {
    return {
        time::MonotonicDuration{10},
        time::MonotonicDuration{10},
        time::MonotonicDuration{10},
        time::MonotonicDuration{10}};
  }

  [[nodiscard]] bench::BenchComponentStatus apply(
      bench::BenchElectricalState) noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus wake_pulse(
      time::MonotonicDuration) noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus read_feedback(
      bench::BenchElectricalFeedback& feedback) noexcept override {
    feedback.state_valid = true;
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus safe_off() noexcept override {
    return bench::BenchComponentStatus::ok;
  }

  bench::BenchElectricalCapabilityMask capability_mask{
      bench::bench_electrical_capability_mask(
          bench::BenchElectricalCapability::power) |
      bench::bench_electrical_capability_mask(
          bench::BenchElectricalCapability::ignition) |
      bench::bench_electrical_capability_mask(
          bench::BenchElectricalCapability::wake_level) |
      bench::bench_electrical_capability_mask(
          bench::BenchElectricalCapability::wake_pulse) |
      bench::bench_electrical_capability_mask(
          bench::BenchElectricalCapability::state_feedback)};
};

domain::DutDescriptor dut(
    const domain::DutProfileId id,
    const domain::DutCapabilityMask caps) {
  return {id, domain::DutClass::actuator, domain::kTruck, caps};
}

domain::DutCapabilityMask cap(const domain::DutCapability value) {
  return domain::dut_capability_mask(value);
}

bench::BenchSessionConfig base(
    const runtime::DutHandle handle) {
  bench::BenchSessionConfig config{};
  config.session_owner = 1U;
  config.dut = handle;
  return config;
}

}  // namespace

int main() {
  int failures = 0;

  runtime::ResourceManager resources;
  runtime::DutRegistry duts;
  const auto basic = duts.register_dut(
      dut(0x4001U, cap(domain::DutCapability::raw_can)));
  const auto env_required = duts.register_dut(
      dut(
          0x4002U,
          cap(domain::DutCapability::raw_can) |
              cap(domain::DutCapability::requires_environment)));
  const auto power_required = duts.register_dut(
      dut(
          0x4003U,
          cap(domain::DutCapability::raw_can) |
              cap(domain::DutCapability::requires_power_control)));
  const auto wake_required = duts.register_dut(
      dut(
          0x4004U,
          cap(domain::DutCapability::raw_can) |
              cap(domain::DutCapability::requires_wake)));

  Endpoint endpoint;
  Environment environment;
  Electrical electrical;

  bench::BenchSession before_freeze{
      resources,
      duts,
      endpoint,
      &electrical,
      &environment};

  auto config = base(basic.handle);
  failures += require(
      before_freeze.validate_configuration(config) ==
          bench::BenchSessionConfigValidationStatus::dut_registry_not_frozen,
      "preflight rejects mutable DUT topology");

  failures += require(
      basic.status == runtime::DutRegistrationStatus::registered &&
          env_required.status == runtime::DutRegistrationStatus::registered &&
          power_required.status == runtime::DutRegistrationStatus::registered &&
          wake_required.status == runtime::DutRegistrationStatus::registered &&
          duts.freeze_configuration(),
      "validation DUT topology freezes");

  bench::BenchSession session{
      resources,
      duts,
      endpoint,
      &electrical,
      &environment};

  config = base(basic.handle);
  failures += require(
      session.validate_configuration(config) ==
          bench::BenchSessionConfigValidationStatus::valid,
      "minimal resolved handoff validates");

  auto invalid = config;
  invalid.session_owner = 0U;
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::invalid_session_owner,
      "preflight reports invalid owner");

  invalid = config;
  invalid.dut = {};
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::invalid_dut_handle,
      "preflight reports invalid DUT handle");

  invalid = config;
  invalid.dut = {63U, 65535U};
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::dut_not_found,
      "preflight reports stale or unknown DUT handle");

  invalid = config;
  invalid.additional_resource_count =
      static_cast<std::uint8_t>(
          bench::BenchSessionConfig::kMaxAdditionalResources + 1U);
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::too_many_resources,
      "preflight rejects resource overflow");

  invalid = config;
  invalid.additional_resources[0U] = {
      runtime::ResourceClass::device_under_test,
      55U};
  invalid.additional_resource_count = 1U;
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::forbidden_dut_resource,
      "profile handoff cannot request second DUT resource");

  invalid = config;
  invalid.additional_resources[0U] = {
      runtime::ResourceClass::can_channel,
      1U};
  invalid.additional_resources[1U] =
      invalid.additional_resources[0U];
  invalid.additional_resource_count = 2U;
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::duplicate_resource,
      "preflight rejects duplicate resource requirements");

  invalid = config;
  invalid.run_electrical_state.power = true;
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::
              electrical_request_without_control,
      "electrical request cannot bypass adapter boundary");

  bench::BenchSession no_electrical{
      resources,
      duts,
      endpoint,
      nullptr,
      &environment};
  invalid = config;
  invalid.use_electrical_control = true;
  invalid.run_electrical_state.power = true;
  failures += require(
      no_electrical.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::
              electrical_control_unavailable,
      "preflight reports missing electrical adapter");

  invalid = config;
  invalid.use_electrical_control = true;
  invalid.run_electrical_state = {false, true, false};
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::invalid_electrical_state,
      "preflight rejects ignition without power");

  Electrical power_only;
  power_only.capability_mask =
      bench::bench_electrical_capability_mask(
          bench::BenchElectricalCapability::power);
  bench::BenchSession limited{
      resources,
      duts,
      endpoint,
      &power_only,
      &environment};
  invalid = config;
  invalid.use_electrical_control = true;
  invalid.run_electrical_state = {true, true, false};
  failures += require(
      limited.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::
              electrical_capability_missing,
      "preflight reports missing electrical capability");

  invalid = config;
  invalid.use_electrical_control = true;
  invalid.run_electrical_state = {true, false, false};
  invalid.use_wake_pulse = true;
  invalid.wake_pulse_width = time::MonotonicDuration{0};
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::invalid_wake_pulse,
      "preflight rejects invalid wake pulse");

  invalid = config;
  invalid.use_electrical_control = true;
  invalid.run_electrical_state = {true, false, false};
  invalid.verify_electrical_state = true;
  power_only.capability_mask =
      bench::bench_electrical_capability_mask(
          bench::BenchElectricalCapability::power);
  failures += require(
      limited.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::
              electrical_feedback_unsupported,
      "preflight reports missing electrical feedback");

  bench::BenchSession no_environment{
      resources,
      duts,
      endpoint,
      &electrical,
      nullptr};
  invalid = base(basic.handle);
  invalid.environment_mode =
      bench::EnvironmentMode::minimal_profile_environment;
  failures += require(
      no_environment.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::environment_unavailable,
      "preflight reports missing environment adapter");

  invalid = base(env_required.handle);
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::environment_required,
      "DUT capability enforces required minimal environment");

  invalid = base(power_required.handle);
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::power_control_required,
      "DUT capability enforces required power control");

  invalid = base(wake_required.handle);
  invalid.use_electrical_control = true;
  invalid.run_electrical_state = {true, false, false};
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::wake_required,
      "DUT capability enforces required wake");

  invalid.environment_mode =
      bench::EnvironmentMode::none;
  invalid.dut = wake_required.handle;
  invalid.use_wake_pulse = true;
  invalid.wake_pulse_width = time::MonotonicDuration{100};
  failures += require(
      session.validate_configuration(invalid) ==
          bench::BenchSessionConfigValidationStatus::valid,
      "valid resolved wake requirement passes neutral preflight");

  if (failures == 0) {
    std::cout << "BENCH_RUNTIME_CONFIG_HANDOFF_V1=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
