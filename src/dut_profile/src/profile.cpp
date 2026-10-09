#include "ecu/dut_profile/profile.hpp"

namespace ecu::dut_profile {

namespace {

constexpr std::uint32_t kMaxStandardCanIdentifier = 0x7FFU;
constexpr std::uint32_t kMaxExtendedCanIdentifier = 0x1FFFFFFFU;

[[nodiscard]] constexpr ProtocolRequirementMask known_protocol_mask()
    noexcept {
  return protocol_requirement_mask(ProtocolRequirement::j1939) |
         protocol_requirement_mask(ProtocolRequirement::isotp) |
         protocol_requirement_mask(ProtocolRequirement::uds) |
         protocol_requirement_mask(ProtocolRequirement::isobus);
}

[[nodiscard]] bool valid_resource_class(
    const ecu::core::v2::runtime::ResourceClass value) noexcept {
  return static_cast<std::uint8_t>(value) <=
      static_cast<std::uint8_t>(
          ecu::core::v2::runtime::ResourceClass::custom);
}

[[nodiscard]] const ResourceRequirement* find_resource_requirement(
    const DutProfileDefinition& definition,
    const ResourceRoleId role) noexcept {
  for (std::size_t index = 0U;
       index < definition.resource_count;
       ++index) {
    if (definition.resources[index].role == role) {
      return &definition.resources[index];
    }
  }
  return nullptr;
}

[[nodiscard]] const CanLinkRequirement* find_can_link(
    const DutProfileDefinition& definition,
    const CanLinkId link_id) noexcept {
  for (std::size_t index = 0U;
       index < definition.can_link_count;
       ++index) {
    if (definition.can_links[index].link_id == link_id) {
      return &definition.can_links[index];
    }
  }
  return nullptr;
}

[[nodiscard]] const ResourceBinding* find_resource_binding(
    const DutProfileBinding& binding,
    const ResourceRoleId role) noexcept {
  for (std::size_t index = 0U;
       index < binding.resource_count;
       ++index) {
    if (binding.resources[index].role == role) {
      return &binding.resources[index];
    }
  }
  return nullptr;
}

[[nodiscard]] bool descriptor_equal(
    const ecu::core::v2::domain::DutDescriptor& left,
    const ecu::core::v2::domain::DutDescriptor& right) noexcept {
  return left.profile_id == right.profile_id &&
         left.dut_class == right.dut_class &&
         left.domains == right.domains &&
         left.capabilities == right.capabilities;
}

[[nodiscard]] bool can_related_capability(
    const ecu::core::v2::domain::DutCapabilityMask capabilities) noexcept {
  using Capability = ecu::core::v2::domain::DutCapability;
  return ecu::core::v2::domain::has_dut_capability(
             capabilities, Capability::raw_can) ||
         ecu::core::v2::domain::has_dut_capability(
             capabilities, Capability::cyclic_can) ||
         ecu::core::v2::domain::has_dut_capability(
             capabilities, Capability::can_fd) ||
         ecu::core::v2::domain::has_dut_capability(
             capabilities, Capability::j1939) ||
         ecu::core::v2::domain::has_dut_capability(
             capabilities, Capability::isotp) ||
         ecu::core::v2::domain::has_dut_capability(
             capabilities, Capability::isobus);
}

[[nodiscard]] bool protocol_capabilities_match(
    const DutProfileDefinition& definition) noexcept {
  using Capability = ecu::core::v2::domain::DutCapability;

  if ((definition.required_protocols & ~known_protocol_mask()) != 0U) {
    return false;
  }

  const struct {
    ProtocolRequirement protocol;
    Capability capability;
  } mappings[] = {
      {ProtocolRequirement::j1939, Capability::j1939},
      {ProtocolRequirement::isotp, Capability::isotp},
      {ProtocolRequirement::uds, Capability::uds},
      {ProtocolRequirement::isobus, Capability::isobus},
  };

  for (const auto& mapping : mappings) {
    if (has_protocol_requirement(
            definition.required_protocols,
            mapping.protocol) &&
        !ecu::core::v2::domain::has_dut_capability(
            definition.dut.capabilities,
            mapping.capability)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] bool valid_rx_expectation(
    const CanRxExpectation& expectation) noexcept {
  if (expectation.link_id == 0U ||
      (!expectation.match_standard &&
       !expectation.match_extended)) {
    return false;
  }

  if (expectation.match_standard) {
    return expectation.identifier <= kMaxStandardCanIdentifier &&
           expectation.mask <= kMaxStandardCanIdentifier;
  }

  return expectation.identifier <= kMaxExtendedCanIdentifier &&
         expectation.mask <= kMaxExtendedCanIdentifier;
}

[[nodiscard]] bool is_power_resource_class(
    const ecu::core::v2::runtime::ResourceClass value) noexcept {
  return value ==
             ecu::core::v2::runtime::ResourceClass::power_domain ||
         value ==
             ecu::core::v2::runtime::ResourceClass::hardware_io;
}

[[nodiscard]] bool valid_cyclic_requirement(
    const CyclicControlRequirement& cyclic) noexcept {
  if (!cyclic.enabled) {
    return cyclic.link_id == 0U &&
           cyclic.period.count() == 0 &&
           cyclic.max_lateness.count() == 0 &&
           cyclic.command_timeout.count() == 0 &&
           cyclic.feedback_timeout.count() == 0;
  }

  return cyclic.link_id != 0U &&
         cyclic.period.count() > 0 &&
         cyclic.max_lateness.count() >= 0 &&
         cyclic.max_lateness < cyclic.period &&
         cyclic.command_timeout.count() > 0 &&
         cyclic.feedback_timeout.count() >= 0;
}

}  // namespace

ProfileValidationStatus validate_profile_definition(
    const DutProfileDefinition& definition) noexcept {
  using Capability = ecu::core::v2::domain::DutCapability;

  if (definition.schema_version != DutProfileDefinition::kSchemaVersion) {
    return ProfileValidationStatus::unsupported_schema;
  }
  if (definition.profile_revision == 0U) {
    return ProfileValidationStatus::invalid_profile_revision;
  }
  if (!ecu::core::v2::domain::is_valid_dut_descriptor(
          definition.dut)) {
    return ProfileValidationStatus::invalid_dut_descriptor;
  }

  if (definition.resource_count > definition.resources.size() ||
      definition.can_link_count > definition.can_links.size() ||
      definition.rx_expectation_count >
          definition.rx_expectations.size()) {
    return ProfileValidationStatus::count_overflow;
  }

  for (std::size_t index = 0U;
       index < definition.resource_count;
       ++index) {
    const auto resource = definition.resources[index];
    if (resource.role == 0U ||
        !valid_resource_class(resource.resource_class)) {
      return ProfileValidationStatus::invalid_resource_role;
    }
    if (resource.resource_class ==
        ecu::core::v2::runtime::ResourceClass::device_under_test) {
      return ProfileValidationStatus::forbidden_dut_resource;
    }

    for (std::size_t other = index + 1U;
         other < definition.resource_count;
         ++other) {
      if (resource.role == definition.resources[other].role) {
        return ProfileValidationStatus::duplicate_resource_role;
      }
    }
  }

  bool has_fd_link = false;
  for (std::size_t index = 0U;
       index < definition.can_link_count;
       ++index) {
    const auto link = definition.can_links[index];
    if (link.link_id == 0U ||
        link.resource_role == 0U ||
        link.nominal_bitrate == 0U ||
        (link.mode != ecu::core::v2::transport::CanMode::normal &&
         link.mode !=
             ecu::core::v2::transport::CanMode::listen_only) ||
        (link.fd_enabled && link.data_bitrate == 0U) ||
        (!link.fd_enabled && link.data_bitrate != 0U)) {
      return ProfileValidationStatus::invalid_can_link;
    }

    for (std::size_t other = index + 1U;
         other < definition.can_link_count;
         ++other) {
      if (link.link_id == definition.can_links[other].link_id) {
        return ProfileValidationStatus::duplicate_can_link;
      }
    }

    const auto* resource =
        find_resource_requirement(definition, link.resource_role);
    if (resource == nullptr) {
      return ProfileValidationStatus::can_resource_missing;
    }
    if (resource->resource_class !=
        ecu::core::v2::runtime::ResourceClass::can_channel) {
      return ProfileValidationStatus::can_resource_class_mismatch;
    }
    has_fd_link = has_fd_link || link.fd_enabled;
  }

  if ((can_related_capability(definition.dut.capabilities) ||
       definition.required_protocols != 0U ||
       definition.rx_expectation_count != 0U ||
       definition.cyclic_control.enabled) &&
      definition.can_link_count == 0U) {
    return ProfileValidationStatus::can_link_required;
  }

  const bool can_fd_capability =
      ecu::core::v2::domain::has_dut_capability(
          definition.dut.capabilities,
          Capability::can_fd);
  if (has_fd_link != can_fd_capability) {
    return ProfileValidationStatus::can_fd_capability_mismatch;
  }

  if (!protocol_capabilities_match(definition)) {
    return ProfileValidationStatus::protocol_capability_mismatch;
  }

  if (has_protocol_requirement(
          definition.required_protocols,
          ProtocolRequirement::uds) &&
      !has_protocol_requirement(
          definition.required_protocols,
          ProtocolRequirement::isotp)) {
    return ProfileValidationStatus::uds_requires_isotp;
  }

  if (has_protocol_requirement(
          definition.required_protocols,
          ProtocolRequirement::isobus) &&
      !has_protocol_requirement(
          definition.required_protocols,
          ProtocolRequirement::j1939)) {
    return ProfileValidationStatus::isobus_requires_j1939;
  }

  for (std::size_t index = 0U;
       index < definition.rx_expectation_count;
       ++index) {
    const auto expectation = definition.rx_expectations[index];
    if (!valid_rx_expectation(expectation)) {
      return ProfileValidationStatus::invalid_rx_expectation;
    }
    if (find_can_link(definition, expectation.link_id) == nullptr) {
      return ProfileValidationStatus::rx_link_missing;
    }
  }

  const auto& electrical = definition.electrical;
  const bool requested = electrical_requested(electrical);
  if (!requested) {
    if (electrical.resource_role != 0U ||
        electrical.wake_pulse_width.count() != 0) {
      return ProfileValidationStatus::invalid_electrical_requirement;
    }
  } else {
    if (electrical.resource_role == 0U ||
        !ecu::bench::is_valid_electrical_state(
            electrical.run_state) ||
        (electrical.use_wake_pulse &&
         (!electrical.run_state.power ||
          electrical.wake_pulse_width.count() <= 0)) ||
        (!electrical.use_wake_pulse &&
         electrical.wake_pulse_width.count() != 0)) {
      return ProfileValidationStatus::invalid_electrical_requirement;
    }

    const auto* power_resource =
        find_resource_requirement(
            definition,
            electrical.resource_role);
    if (power_resource == nullptr) {
      return ProfileValidationStatus::electrical_resource_missing;
    }
    if (!is_power_resource_class(
            power_resource->resource_class)) {
      return ProfileValidationStatus::
          electrical_resource_class_mismatch;
    }
  }

  const bool requires_power =
      ecu::core::v2::domain::has_dut_capability(
          definition.dut.capabilities,
          Capability::requires_power_control);
  const bool requires_wake =
      ecu::core::v2::domain::has_dut_capability(
          definition.dut.capabilities,
          Capability::requires_wake);

  if (requires_power != requested ||
      (requires_power && !electrical.run_state.power) ||
      requires_wake !=
          (electrical.run_state.wake ||
           electrical.use_wake_pulse)) {
    return ProfileValidationStatus::invalid_electrical_requirement;
  }

  const bool requires_environment =
      ecu::core::v2::domain::has_dut_capability(
          definition.dut.capabilities,
          Capability::requires_environment);
  const bool uses_environment =
      definition.environment_mode ==
      ecu::bench::EnvironmentMode::minimal_profile_environment;
  if (requires_environment != uses_environment) {
    return ProfileValidationStatus::environment_requirement_mismatch;
  }

  if (!valid_cyclic_requirement(definition.cyclic_control)) {
    return ProfileValidationStatus::
        cyclic_control_requirement_mismatch;
  }

  const bool cyclic_active =
      ecu::core::v2::domain::has_dut_capability(
          definition.dut.capabilities,
          Capability::cyclic_can) &&
      ecu::core::v2::domain::has_dut_capability(
          definition.dut.capabilities,
          Capability::active_control);

  if (cyclic_active != definition.cyclic_control.enabled) {
    return ProfileValidationStatus::
        cyclic_control_requirement_mismatch;
  }

  if (definition.cyclic_control.enabled) {
    if (find_can_link(
            definition,
            definition.cyclic_control.link_id) == nullptr) {
      return ProfileValidationStatus::
          cyclic_control_requirement_mismatch;
    }

    if (definition.cyclic_control.feedback_timeout.count() > 0 &&
        !ecu::core::v2::domain::has_dut_capability(
            definition.dut.capabilities,
            Capability::feedback)) {
      return ProfileValidationStatus::
          cyclic_control_requirement_mismatch;
    }
  }

  return ProfileValidationStatus::valid;
}

ProfileResolveStatus resolve_profile_session(
    const DutProfileDefinition& definition,
    const ecu::core::v2::runtime::DutRegistry& duts,
    const DutProfileBinding& binding,
    ResolvedDutSessionPlan& plan) noexcept {
  plan = {};

  if (validate_profile_definition(definition) !=
      ProfileValidationStatus::valid) {
    return ProfileResolveStatus::invalid_profile;
  }

  if (binding.session_owner == 0U ||
      !binding.dut.valid() ||
      binding.resource_count > binding.resources.size() ||
      (definition.can_link_count != 0U &&
       !binding.timestamp_domain.valid())) {
    return ProfileResolveStatus::invalid_binding;
  }

  if (duts.configuration_state() !=
      ecu::core::v2::runtime::ConfigurationState::frozen) {
    return ProfileResolveStatus::dut_registry_not_frozen;
  }

  const auto* registered = duts.get(binding.dut);
  if (registered == nullptr) {
    return ProfileResolveStatus::dut_not_found;
  }
  if (!descriptor_equal(*registered, definition.dut)) {
    return ProfileResolveStatus::dut_descriptor_mismatch;
  }

  for (std::size_t index = 0U;
       index < binding.resource_count;
       ++index) {
    const auto& current = binding.resources[index];
    if (current.role == 0U) {
      return ProfileResolveStatus::invalid_binding;
    }

    const auto* requirement =
        find_resource_requirement(definition, current.role);
    if (requirement == nullptr) {
      return ProfileResolveStatus::unknown_binding_role;
    }
    if (requirement->resource_class !=
        current.resource.resource_class) {
      return ProfileResolveStatus::binding_class_mismatch;
    }

    for (std::size_t other = index + 1U;
         other < binding.resource_count;
         ++other) {
      if (current.role == binding.resources[other].role) {
        return ProfileResolveStatus::duplicate_binding_role;
      }
      if (current.resource == binding.resources[other].resource) {
        return ProfileResolveStatus::duplicate_bound_resource;
      }
    }
  }

  for (std::size_t index = 0U;
       index < definition.resource_count;
       ++index) {
    if (find_resource_binding(
            binding,
            definition.resources[index].role) == nullptr) {
      return ProfileResolveStatus::missing_binding;
    }
  }

  plan.schema_version = definition.schema_version;
  plan.profile_revision = definition.profile_revision;
  plan.profile_id = definition.dut.profile_id;
  plan.required_protocols = definition.required_protocols;
  plan.cyclic_control = definition.cyclic_control;

  plan.bench.session_owner = binding.session_owner;
  plan.bench.dut = binding.dut;
  plan.bench.additional_resource_count =
      definition.resource_count;

  for (std::size_t index = 0U;
       index < definition.resource_count;
       ++index) {
    const auto* resolved =
        find_resource_binding(
            binding,
            definition.resources[index].role);
    if (resolved == nullptr) {
      plan = {};
      return ProfileResolveStatus::missing_binding;
    }
    plan.bench.additional_resources[index] =
        resolved->resource;
  }

  plan.bench.use_electrical_control =
      electrical_requested(definition.electrical);
  plan.bench.run_electrical_state =
      definition.electrical.run_state;
  plan.bench.use_wake_pulse =
      definition.electrical.use_wake_pulse;
  plan.bench.wake_pulse_width =
      definition.electrical.wake_pulse_width;
  plan.bench.verify_electrical_state =
      definition.electrical.verify_state;
  plan.bench.environment_mode =
      definition.environment_mode;

  plan.rx_expectation_count = definition.rx_expectation_count;
  for (std::size_t index = 0U;
       index < definition.rx_expectation_count;
       ++index) {
    plan.rx_expectations[index] = definition.rx_expectations[index];
  }

  plan.can_link_count = definition.can_link_count;
  for (std::size_t index = 0U;
       index < definition.can_link_count;
       ++index) {
    const auto& requirement = definition.can_links[index];
    const auto* resource =
        find_resource_binding(
            binding,
            requirement.resource_role);
    if (resource == nullptr) {
      plan = {};
      return ProfileResolveStatus::missing_binding;
    }

    auto& resolved = plan.can_links[index];
    resolved.link_id = requirement.link_id;
    resolved.resource = resource->resource;
    resolved.channel_config = {
        requirement.nominal_bitrate,
        requirement.fd_enabled,
        requirement.data_bitrate,
        requirement.mode,
        binding.timestamp_domain};

    if (!ecu::core::v2::transport::is_valid_can_channel_config(
            resolved.channel_config)) {
      plan = {};
      return ProfileResolveStatus::invalid_can_channel_config;
    }
  }

  return ProfileResolveStatus::resolved;
}

}  // namespace ecu::dut_profile
