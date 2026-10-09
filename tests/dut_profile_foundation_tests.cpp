#include "ecu/dut_profile/profile.hpp"

#include <cstdint>
#include <iostream>

namespace {

namespace bench = ecu::bench;
namespace domain = ecu::core::v2::domain;
namespace dp = ecu::dut_profile;
namespace runtime = ecu::core::v2::runtime;
namespace time = ecu::core::v2::time;
namespace transport = ecu::core::v2::transport;

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

[[nodiscard]] domain::DutCapabilityMask cap(
    const domain::DutCapability value) noexcept {
  return domain::dut_capability_mask(value);
}

[[nodiscard]] dp::DutProfileDefinition diagnostic_profile() {
  dp::DutProfileDefinition profile{};
  profile.profile_revision = 1U;
  profile.dut = {
      0x6001U,
      domain::DutClass::ecu,
      domain::kTruck,
      static_cast<domain::DutCapabilityMask>(
          cap(domain::DutCapability::raw_can) |
          cap(domain::DutCapability::isotp) |
          cap(domain::DutCapability::uds))};
  profile.required_protocols =
      dp::protocol_requirement_mask(
          dp::ProtocolRequirement::isotp) |
      dp::protocol_requirement_mask(
          dp::ProtocolRequirement::uds);
  profile.resources[0U] = {
      10U,
      runtime::ResourceClass::can_channel};
  profile.resource_count = 1U;
  profile.can_links[0U] = {
      1U,
      10U,
      500000U,
      false,
      0U,
      transport::CanMode::normal};
  profile.can_link_count = 1U;
  profile.rx_expectations[0U] = {
      1U,
      0x7E8U,
      0x7FFU,
      true,
      false};
  profile.rx_expectation_count = 1U;
  return profile;
}

[[nodiscard]] dp::DutProfileDefinition actuator_profile() {
  dp::DutProfileDefinition profile{};
  profile.profile_revision = 3U;
  profile.dut = {
      0x6002U,
      domain::DutClass::actuator,
      domain::kTruck | domain::kOhv,
      static_cast<domain::DutCapabilityMask>(
          cap(domain::DutCapability::raw_can) |
          cap(domain::DutCapability::cyclic_can) |
          cap(domain::DutCapability::active_control) |
          cap(domain::DutCapability::feedback) |
          cap(domain::DutCapability::requires_environment) |
          cap(domain::DutCapability::requires_power_control) |
          cap(domain::DutCapability::requires_wake))};
  profile.resources[0U] = {
      20U,
      runtime::ResourceClass::can_channel};
  profile.resources[1U] = {
      21U,
      runtime::ResourceClass::power_domain};
  profile.resource_count = 2U;
  profile.can_links[0U] = {
      1U,
      20U,
      250000U,
      false,
      0U,
      transport::CanMode::normal};
  profile.can_link_count = 1U;
  profile.rx_expectations[0U] = {
      1U,
      0x180U,
      0x7FFU,
      true,
      false};
  profile.rx_expectation_count = 1U;
  profile.electrical.resource_role = 21U;
  profile.electrical.run_state = {true, false, false};
  profile.electrical.use_wake_pulse = true;
  profile.electrical.wake_pulse_width =
      time::MonotonicDuration{100};
  profile.electrical.verify_state = true;
  profile.environment_mode =
      bench::EnvironmentMode::minimal_profile_environment;
  profile.cyclic_control.enabled = true;
  profile.cyclic_control.link_id = 1U;
  profile.cyclic_control.period =
      time::MonotonicDuration{10000000};
  profile.cyclic_control.max_lateness =
      time::MonotonicDuration{1000000};
  profile.cyclic_control.command_timeout =
      time::MonotonicDuration{50000000};
  profile.cyclic_control.feedback_timeout =
      time::MonotonicDuration{50000000};
  return profile;
}

[[nodiscard]] dp::DutProfileBinding diagnostic_binding(
    const runtime::DutHandle handle,
    const runtime::ResourceInstance can_instance) {
  dp::DutProfileBinding binding{};
  binding.session_owner = 700U;
  binding.dut = handle;
  binding.timestamp_domain = {33U};
  binding.resources[0U] = {
      10U,
      {runtime::ResourceClass::can_channel, can_instance}};
  binding.resource_count = 1U;
  return binding;
}

}  // namespace

int main() {
  int failures = 0;

  {
    const auto profile = diagnostic_profile();
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::valid,
        "diagnostic-style DUT-neutral profile validates");
  }

  {
    const auto profile = actuator_profile();
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::valid,
        "actuator-style DUT-neutral profile validates");
  }

  {
    auto profile = diagnostic_profile();
    profile.schema_version = 99U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::unsupported_schema,
        "unknown profile schema fails closed");

    profile = diagnostic_profile();
    profile.profile_revision = 0U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::invalid_profile_revision,
        "zero profile revision is rejected");

    profile = diagnostic_profile();
    profile.resources[1U] = profile.resources[0U];
    profile.resource_count = 2U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::duplicate_resource_role,
        "resource roles are unique");

    profile = diagnostic_profile();
    profile.resources[0U].resource_class =
        runtime::ResourceClass::device_under_test;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::forbidden_dut_resource,
        "profile cannot request a second DUT resource");

    profile = diagnostic_profile();
    profile.can_links[0U].resource_role = 99U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::can_resource_missing,
        "CAN link must reference a declared logical resource");

    profile = diagnostic_profile();
    profile.resources[0U].resource_class =
        runtime::ResourceClass::storage;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::can_resource_class_mismatch,
        "CAN link role must resolve to CAN resource class");

    profile = diagnostic_profile();
    profile.can_link_count = 0U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::can_link_required,
        "CAN-oriented profile requires at least one CAN link");

    profile = diagnostic_profile();
    profile.can_links[0U].fd_enabled = true;
    profile.can_links[0U].data_bitrate = 2000000U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::can_fd_capability_mismatch,
        "CAN-FD link and DUT capability must agree");

    profile = diagnostic_profile();
    profile.required_protocols |=
        dp::protocol_requirement_mask(
            dp::ProtocolRequirement::j1939);
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::protocol_capability_mismatch,
        "required protocol must be declared as DUT capability");

    profile = diagnostic_profile();
    profile.required_protocols =
        dp::protocol_requirement_mask(
            dp::ProtocolRequirement::uds);
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::uds_requires_isotp,
        "UDS requirement is transport-explicit in current CAN profile schema");

    profile = diagnostic_profile();
    profile.dut.capabilities =
        static_cast<domain::DutCapabilityMask>(
            profile.dut.capabilities |
            cap(domain::DutCapability::j1939) |
            cap(domain::DutCapability::isobus));
    profile.required_protocols =
        dp::protocol_requirement_mask(
            dp::ProtocolRequirement::isobus);
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::isobus_requires_j1939,
        "ISOBUS requirement declares its J1939 network foundation");

    profile = diagnostic_profile();
    profile.rx_expectations[0U].link_id = 2U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::rx_link_missing,
        "expected RX filter must reference a declared CAN link");

    profile = diagnostic_profile();
    profile.rx_expectations[0U].identifier = 0x800U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::invalid_rx_expectation,
        "standard CAN expectation rejects out-of-range identifier");
  }

  {
    auto profile = actuator_profile();
    profile.resources[1U].resource_class =
        runtime::ResourceClass::storage;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::
                electrical_resource_class_mismatch,
        "electrical role requires power-domain or hardware-I/O resource");

    profile = actuator_profile();
    profile.electrical.use_wake_pulse = false;
    profile.electrical.wake_pulse_width =
        time::MonotonicDuration{0};
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::invalid_electrical_requirement,
        "required wake cannot disappear from resolved profile definition");

    profile = actuator_profile();
    profile.environment_mode = bench::EnvironmentMode::none;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::
                environment_requirement_mismatch,
        "environment capability and profile requirement remain consistent");

    profile = actuator_profile();
    profile.cyclic_control.enabled = false;
    profile.cyclic_control.link_id = 0U;
    profile.cyclic_control.period = {};
    profile.cyclic_control.max_lateness = {};
    profile.cyclic_control.command_timeout = {};
    profile.cyclic_control.feedback_timeout = {};
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::
                cyclic_control_requirement_mismatch,
        "active cyclic DUT must use generic cyclic runtime contract");
  }

  runtime::DutRegistry duts;
  const auto diagnostic = diagnostic_profile();
  const auto actuator = actuator_profile();
  const auto diagnostic_registration =
      duts.register_dut(diagnostic.dut);
  const auto actuator_registration =
      duts.register_dut(actuator.dut);

  failures += require(
      diagnostic_registration.status ==
              runtime::DutRegistrationStatus::registered &&
          actuator_registration.status ==
              runtime::DutRegistrationStatus::registered,
      "profile descriptors register in frozen Core DUT registry");

  {
    dp::ResolvedDutSessionPlan plan{};
    const auto binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::dut_registry_not_frozen,
        "profile resolution requires stable frozen DUT topology");
  }

  failures += require(
      duts.freeze_configuration(),
      "Core DUT registry freezes before profile resolution");

  {
    dp::ResolvedDutSessionPlan plan{};
    const auto binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);

    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::resolved,
        "portable profile resolves against bench resource binding");

    failures += require(
        plan.schema_version ==
                dp::DutProfileDefinition::kSchemaVersion &&
            plan.profile_revision == 1U &&
            plan.profile_id == diagnostic.dut.profile_id &&
            plan.required_protocols ==
                diagnostic.required_protocols &&
            plan.bench.session_owner == 700U &&
            plan.bench.dut.slot ==
                diagnostic_registration.handle.slot &&
            plan.bench.additional_resource_count == 1U &&
            plan.bench.additional_resources[0U] ==
                runtime::ResourceKey{
                    runtime::ResourceClass::can_channel,
                    3U} &&
            !plan.bench.use_electrical_control &&
            plan.bench.environment_mode ==
                bench::EnvironmentMode::none &&
            plan.can_link_count == 1U &&
            plan.rx_expectation_count == 1U &&
            plan.rx_expectations[0U].link_id == 1U &&
            plan.rx_expectations[0U].identifier == 0x7E8U &&
            plan.rx_expectations[0U].mask == 0x7FFU &&
            plan.can_links[0U].link_id == 1U &&
            plan.can_links[0U].resource ==
                runtime::ResourceKey{
                    runtime::ResourceClass::can_channel,
                    3U} &&
            plan.can_links[0U].channel_config.nominal_bitrate ==
                500000U &&
            !plan.can_links[0U].channel_config.fd_enabled &&
            plan.can_links[0U].channel_config.timestamp_domain ==
                time::MonotonicClockDomainId{33U},
        "resolved plan contains Bench config and platform-neutral CAN config");

    auto alternate_binding = binding;
    alternate_binding.resources[0U].resource.instance = 8U;
    dp::ResolvedDutSessionPlan alternate{};
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            alternate_binding,
            alternate) ==
                dp::ProfileResolveStatus::resolved &&
            alternate.can_links[0U].resource.instance == 8U &&
            diagnostic.can_links[0U].resource_role == 10U,
        "same portable profile binds to a different physical bench resource");
  }

  {
    dp::ResolvedDutSessionPlan plan{};
    auto binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);

    binding.session_owner = 0U;
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::invalid_binding,
        "zero session owner is rejected by resolver");

    binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);
    binding.timestamp_domain = {};
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::invalid_binding,
        "CAN profile requires resolved monotonic timestamp domain");

    binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);
    binding.resource_count = 0U;
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::missing_binding,
        "missing logical resource binding is explicit");

    binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);
    binding.resources[1U] = {
        99U,
        {runtime::ResourceClass::can_channel, 4U}};
    binding.resource_count = 2U;
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::unknown_binding_role,
        "unknown resource binding role fails closed");

    binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);
    binding.resources[0U].resource.resource_class =
        runtime::ResourceClass::storage;
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::binding_class_mismatch,
        "binding cannot change declared resource class");

    binding = diagnostic_binding(
        diagnostic_registration.handle,
        3U);
    binding.resources[1U] = binding.resources[0U];
    binding.resource_count = 2U;
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::duplicate_binding_role,
        "duplicate logical binding role is rejected");

    binding = diagnostic_binding(
        actuator_registration.handle,
        3U);
    failures += require(
        dp::resolve_profile_session(
            diagnostic,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::dut_descriptor_mismatch,
        "profile cannot be resolved against a different registered DUT");
  }

  {
    auto profile = diagnostic_profile();
    profile.resources[1U] = {
        11U,
        runtime::ResourceClass::can_channel};
    profile.resource_count = 2U;
    failures += require(
        dp::validate_profile_definition(profile) ==
            dp::ProfileValidationStatus::valid,
        "additional logical platform resource may remain profile-declared");

    runtime::DutRegistry local_duts;
    const auto registered = local_duts.register_dut(profile.dut);
    failures += require(
        registered.status ==
                runtime::DutRegistrationStatus::registered &&
            local_duts.freeze_configuration(),
        "duplicate-resource resolver fixture freezes");

    dp::DutProfileBinding binding{};
    binding.session_owner = 701U;
    binding.dut = registered.handle;
    binding.timestamp_domain = {44U};
    binding.resources[0U] = {
        10U,
        {runtime::ResourceClass::can_channel, 9U}};
    binding.resources[1U] = {
        11U,
        {runtime::ResourceClass::can_channel, 9U}};
    binding.resource_count = 2U;

    dp::ResolvedDutSessionPlan plan{};
    failures += require(
        dp::resolve_profile_session(
            profile,
            local_duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::duplicate_bound_resource,
        "two logical roles cannot silently own the same bench resource");
  }

  {
    dp::DutProfileBinding binding{};
    binding.session_owner = 702U;
    binding.dut = actuator_registration.handle;
    binding.timestamp_domain = {55U};
    binding.resources[0U] = {
        20U,
        {runtime::ResourceClass::can_channel, 5U}};
    binding.resources[1U] = {
        21U,
        {runtime::ResourceClass::power_domain, 2U}};
    binding.resource_count = 2U;

    dp::ResolvedDutSessionPlan plan{};
    failures += require(
        dp::resolve_profile_session(
            actuator,
            duts,
            binding,
            plan) ==
            dp::ProfileResolveStatus::resolved,
        "actuator-style profile resolves without concrete OEM semantics");

    failures += require(
        plan.bench.additional_resource_count == 2U &&
            plan.bench.use_electrical_control &&
            plan.bench.run_electrical_state.power &&
            plan.bench.use_wake_pulse &&
            plan.bench.wake_pulse_width ==
                time::MonotonicDuration{100} &&
            plan.bench.verify_electrical_state &&
            plan.bench.environment_mode ==
                bench::EnvironmentMode::
                    minimal_profile_environment &&
            plan.cyclic_control.enabled &&
            plan.cyclic_control.link_id == 1U,
        "resolved actuator plan carries electrical/environment/cyclic requirements");
  }

  if (failures == 0) {
    std::cout << "DUT_PROFILE_FOUNDATION_V1=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
