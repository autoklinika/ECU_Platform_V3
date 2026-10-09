#pragma once

#include "ecu/bench/session.hpp"
#include "ecu/core_v2/domain/device_under_test.hpp"
#include "ecu/core_v2/runtime/dut_registry.hpp"
#include "ecu/core_v2/runtime/resource_manager.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/can_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::dut_profile {

using ResourceRoleId = std::uint16_t;
using CanLinkId = std::uint8_t;
using ProtocolRequirementMask = std::uint32_t;

enum class ProtocolRequirement : std::uint8_t {
  j1939 = 0U,
  isotp = 1U,
  uds = 2U,
  isobus = 3U,
};

[[nodiscard]] constexpr ProtocolRequirementMask protocol_requirement_mask(
    const ProtocolRequirement protocol) noexcept {
  const auto bit = static_cast<std::uint8_t>(protocol);
  return bit < 32U
             ? static_cast<ProtocolRequirementMask>(
                   static_cast<ProtocolRequirementMask>(1U) << bit)
             : 0U;
}

[[nodiscard]] constexpr bool has_protocol_requirement(
    const ProtocolRequirementMask mask,
    const ProtocolRequirement protocol) noexcept {
  const auto required = protocol_requirement_mask(protocol);
  return required != 0U && (mask & required) == required;
}

struct ResourceRequirement {
  ResourceRoleId role{0U};
  ecu::core::v2::runtime::ResourceClass resource_class{
      ecu::core::v2::runtime::ResourceClass::custom};
};

struct CanLinkRequirement {
  CanLinkId link_id{0U};
  ResourceRoleId resource_role{0U};
  std::uint32_t nominal_bitrate{0U};
  bool fd_enabled{false};
  std::uint32_t data_bitrate{0U};
  ecu::core::v2::transport::CanMode mode{
      ecu::core::v2::transport::CanMode::normal};
};

struct CanRxExpectation {
  CanLinkId link_id{0U};
  std::uint32_t identifier{0U};
  std::uint32_t mask{0U};
  bool match_standard{true};
  bool match_extended{true};
};

struct ElectricalRequirement {
  ResourceRoleId resource_role{0U};
  ecu::bench::BenchElectricalState run_state{};
  bool use_wake_pulse{false};
  ecu::core::v2::time::MonotonicDuration wake_pulse_width{0};
  bool verify_state{false};
};

struct CyclicControlRequirement {
  bool enabled{false};
  CanLinkId link_id{0U};
  ecu::core::v2::time::MonotonicDuration period{0};
  ecu::core::v2::time::MonotonicDuration max_lateness{0};
  ecu::core::v2::time::MonotonicDuration command_timeout{0};
  ecu::core::v2::time::MonotonicDuration feedback_timeout{0};
};

struct DutProfileDefinition {
  static constexpr std::uint16_t kSchemaVersion = 1U;
  static constexpr std::size_t kMaxResourceRequirements =
      ecu::bench::BenchSessionConfig::kMaxAdditionalResources;
  static constexpr std::size_t kMaxCanLinks = 4U;
  static constexpr std::size_t kMaxRxExpectations = 16U;

  std::uint16_t schema_version{kSchemaVersion};
  std::uint32_t profile_revision{0U};
  ecu::core::v2::domain::DutDescriptor dut{};
  ProtocolRequirementMask required_protocols{0U};

  std::array<ResourceRequirement, kMaxResourceRequirements>
      resources{};
  std::uint8_t resource_count{0U};

  std::array<CanLinkRequirement, kMaxCanLinks> can_links{};
  std::uint8_t can_link_count{0U};

  std::array<CanRxExpectation, kMaxRxExpectations>
      rx_expectations{};
  std::uint8_t rx_expectation_count{0U};

  ElectricalRequirement electrical{};
  ecu::bench::EnvironmentMode environment_mode{
      ecu::bench::EnvironmentMode::none};
  CyclicControlRequirement cyclic_control{};
};

enum class ProfileValidationStatus : std::uint8_t {
  valid,
  unsupported_schema,
  invalid_profile_revision,
  invalid_dut_descriptor,
  count_overflow,
  invalid_resource_role,
  duplicate_resource_role,
  forbidden_dut_resource,
  invalid_can_link,
  duplicate_can_link,
  can_resource_missing,
  can_resource_class_mismatch,
  can_link_required,
  can_fd_capability_mismatch,
  protocol_capability_mismatch,
  uds_requires_isotp,
  isobus_requires_j1939,
  invalid_rx_expectation,
  rx_link_missing,
  electrical_resource_missing,
  electrical_resource_class_mismatch,
  invalid_electrical_requirement,
  environment_requirement_mismatch,
  cyclic_control_requirement_mismatch,
};

[[nodiscard]] ProfileValidationStatus validate_profile_definition(
    const DutProfileDefinition& definition) noexcept;

struct ResourceBinding {
  ResourceRoleId role{0U};
  ecu::core::v2::runtime::ResourceKey resource{};
};

struct DutProfileBinding {
  static constexpr std::size_t kMaxResourceBindings =
      DutProfileDefinition::kMaxResourceRequirements;

  ecu::core::v2::runtime::ResourceOwnerId session_owner{0U};
  ecu::core::v2::runtime::DutHandle dut{};
  ecu::core::v2::time::MonotonicClockDomainId timestamp_domain{};

  std::array<ResourceBinding, kMaxResourceBindings> resources{};
  std::uint8_t resource_count{0U};
};

struct ResolvedCanLink {
  CanLinkId link_id{0U};
  ecu::core::v2::runtime::ResourceKey resource{};
  ecu::core::v2::transport::CanChannelConfig channel_config{};
};

struct ResolvedDutSessionPlan {
  static constexpr std::size_t kMaxCanLinks =
      DutProfileDefinition::kMaxCanLinks;
  static constexpr std::size_t kMaxRxExpectations =
      DutProfileDefinition::kMaxRxExpectations;

  std::uint16_t schema_version{
      DutProfileDefinition::kSchemaVersion};
  std::uint32_t profile_revision{0U};
  ecu::core::v2::domain::DutProfileId profile_id{0U};
  ProtocolRequirementMask required_protocols{0U};

  ecu::bench::BenchSessionConfig bench{};
  std::array<ResolvedCanLink, kMaxCanLinks> can_links{};
  std::uint8_t can_link_count{0U};
  std::array<CanRxExpectation, kMaxRxExpectations> rx_expectations{};
  std::uint8_t rx_expectation_count{0U};
  CyclicControlRequirement cyclic_control{};
};

enum class ProfileResolveStatus : std::uint8_t {
  resolved,
  invalid_profile,
  invalid_binding,
  dut_registry_not_frozen,
  dut_not_found,
  dut_descriptor_mismatch,
  duplicate_binding_role,
  unknown_binding_role,
  missing_binding,
  binding_class_mismatch,
  duplicate_bound_resource,
  invalid_can_channel_config,
};

[[nodiscard]] ProfileResolveStatus resolve_profile_session(
    const DutProfileDefinition& definition,
    const ecu::core::v2::runtime::DutRegistry& duts,
    const DutProfileBinding& binding,
    ResolvedDutSessionPlan& plan) noexcept;

[[nodiscard]] constexpr bool electrical_requested(
    const ElectricalRequirement& requirement) noexcept {
  return requirement.run_state.power ||
         requirement.run_state.ignition ||
         requirement.run_state.wake ||
         requirement.use_wake_pulse ||
         requirement.verify_state;
}

}  // namespace ecu::dut_profile
