#include "ecu/dut_profiles/daf_sac/identification_program.hpp"

#include "ecu/core_v2/protocol/uds/uds_services.hpp"

#include <cstddef>
#include <cstdint>

namespace ecu::dut_profiles::daf_sac {
namespace {

[[nodiscard]] std::uint8_t byte_value(const std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] bool active_uds_status(
    const ecu::core::v2::protocol::uds::UdsStatus status) noexcept {
  using Status = ecu::core::v2::protocol::uds::UdsStatus;
  return status == Status::ok ||
         status == Status::idle ||
         status == Status::in_progress;
}

}  // namespace

IdentificationProgram::IdentificationProgram(
    const CanBitrateProfile bitrate,
    ecu::core::v2::protocol::uds::UdsClient& uds,
    const ecu::core::v2::time::IMonotonicClock& clock,
    const ecu::bench::BenchComponentExecutionContract execution) noexcept
    : bitrate_(bitrate),
      uds_(uds),
      clock_(clock),
      execution_(execution) {}

ecu::dut_profile::DutProfileProgramDescriptor
IdentificationProgram::descriptor() const noexcept {
  return {
      ecu::dut_profile::DutProfileDefinition::kSchemaVersion,
      kProfileRevision,
      profile_id(bitrate_)};
}

ecu::bench::BenchComponentExecutionContract
IdentificationProgram::execution_contract() const noexcept {
  return execution_;
}

ecu::bench::BenchComponentStatus IdentificationProgram::prepare(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan) || !uds_.valid()) {
    return fail();
  }

  const auto properties = clock_.properties();
  if (!ecu::core::v2::time::is_valid_clock_properties(properties) ||
      properties.domain !=
          plan.can_links[0U].channel_config.timestamp_domain) {
    return fail();
  }

  uds_.reset();
  result_ = {};
  reply_diagnostic_ = {};
  last_nrc_ = 0U;
  last_uds_status_ =
      ecu::core::v2::protocol::uds::UdsStatus::idle;
  last_transport_failure_ =
      ecu::core::v2::protocol::uds::UdsTransportFailure::none;
  step_ = Step::idle;
  status_ = IdentificationProgramStatus::prepared;
  return ecu::bench::BenchComponentStatus::ok;
}

ecu::bench::BenchComponentStatus IdentificationProgram::activate(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan) ||
      status_ != IdentificationProgramStatus::prepared) {
    return fail();
  }

  result_ = {};
  reply_diagnostic_ = {};
  last_nrc_ = 0U;
  last_uds_status_ =
      ecu::core::v2::protocol::uds::UdsStatus::idle;
  last_transport_failure_ =
      ecu::core::v2::protocol::uds::UdsTransportFailure::none;
  step_ = Step::request_vin;
  status_ = IdentificationProgramStatus::running;
  return ecu::bench::BenchComponentStatus::ok;
}

ecu::bench::BenchComponentStatus IdentificationProgram::service(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan) ||
      (status_ != IdentificationProgramStatus::running &&
       status_ != IdentificationProgramStatus::complete)) {
    return fail();
  }

  if (status_ == IdentificationProgramStatus::complete) {
    return ecu::bench::BenchComponentStatus::no_action;
  }

  const auto now = clock_.read();
  if (!ecu::core::v2::time::is_valid_clock_reading(
          now,
          plan.can_links[0U].channel_config.timestamp_domain)) {
    return fail();
  }

  switch (step_) {
    case Step::request_vin:
      return start_did(kDidVin, Step::wait_vin);

    case Step::wait_vin:
      return service_wait(
          kDidVin,
          result_.vin,
          Step::request_software,
          now);

    case Step::request_software:
      return start_did(
          kDidSoftware,
          Step::wait_software);

    case Step::wait_software:
      return service_wait(
          kDidSoftware,
          result_.software,
          Step::request_hardware,
          now);

    case Step::request_hardware:
      return start_did(
          kDidHardware,
          Step::wait_hardware);

    case Step::wait_hardware:
      return service_wait(
          kDidHardware,
          result_.hardware,
          Step::done,
          now);

    case Step::done:
      status_ = IdentificationProgramStatus::complete;
      return ecu::bench::BenchComponentStatus::no_action;

    case Step::idle:
    case Step::fault:
      return fail();
  }

  return fail();
}

ecu::bench::BenchComponentStatus IdentificationProgram::safe_stop(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan)) {
    return fail();
  }

  uds_.reset();
  step_ = Step::idle;
  if (status_ != IdentificationProgramStatus::fault) {
    status_ = IdentificationProgramStatus::prepared;
  }
  return ecu::bench::BenchComponentStatus::ok;
}

ecu::bench::BenchComponentStatus IdentificationProgram::stop(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan)) {
    return fail();
  }

  uds_.reset();
  reset_state();
  return ecu::bench::BenchComponentStatus::ok;
}

IdentificationProgramStatus IdentificationProgram::status() const noexcept {
  return status_;
}

const IdentificationResult& IdentificationProgram::result() const noexcept {
  return result_;
}

IdentificationReplyDiagnostic IdentificationProgram::last_reply_diagnostic()
    const noexcept {
  return reply_diagnostic_;
}

std::uint8_t IdentificationProgram::last_nrc() const noexcept {
  return last_nrc_;
}

ecu::core::v2::protocol::uds::UdsStatus
IdentificationProgram::last_uds_status() const noexcept {
  return last_uds_status_;
}

ecu::core::v2::protocol::uds::UdsTransportFailure
IdentificationProgram::last_transport_failure() const noexcept {
  return last_transport_failure_;
}

bool IdentificationProgram::plan_matches(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) const noexcept {
  namespace dp = ecu::dut_profile;

  if (plan.schema_version != dp::DutProfileDefinition::kSchemaVersion ||
      plan.profile_revision != kProfileRevision ||
      plan.profile_id != profile_id(bitrate_) ||
      plan.can_link_count != 1U ||
      plan.rx_expectation_count == 0U) {
    return false;
  }

  if (!dp::has_protocol_requirement(
          plan.required_protocols,
          dp::ProtocolRequirement::isotp) ||
      !dp::has_protocol_requirement(
          plan.required_protocols,
          dp::ProtocolRequirement::uds)) {
    return false;
  }

  const auto& link = plan.can_links[0U];
  if (link.link_id != kPrimaryCanLink ||
      link.channel_config.nominal_bitrate != nominal_bitrate(bitrate_) ||
      link.channel_config.fd_enabled ||
      link.channel_config.data_bitrate != 0U ||
      link.channel_config.mode !=
          ecu::core::v2::transport::CanMode::normal ||
      !link.channel_config.timestamp_domain.valid()) {
    return false;
  }

  for (std::size_t index = 0U;
       index < plan.rx_expectation_count;
       ++index) {
    const auto& expectation = plan.rx_expectations[index];
    if (expectation.link_id == kPrimaryCanLink &&
        expectation.identifier == kResponseCanId &&
        expectation.mask == 0x1FFFFFFFU &&
        !expectation.match_standard &&
        expectation.match_extended) {
      return true;
    }
  }

  return false;
}

ecu::bench::BenchComponentStatus IdentificationProgram::start_did(
    const std::uint16_t did,
    const Step wait_step) noexcept {
  reply_diagnostic_ = {};
  reply_diagnostic_.requested_did = did;
  if (uds_.busy() || uds_.has_response()) {
    return fail();
  }

  const auto request =
      ecu::core::v2::protocol::uds::make_read_data_by_identifier(did);
  const auto started = uds_.start_request(request);
  last_uds_status_ = started;
  last_transport_failure_ =
      ecu::core::v2::protocol::uds::UdsTransportFailure::none;
  if (started !=
      ecu::core::v2::protocol::uds::UdsStatus::in_progress) {
    return fail();
  }

  step_ = wait_step;
  return ecu::bench::BenchComponentStatus::ok;
}

ecu::bench::BenchComponentStatus IdentificationProgram::service_wait(
    const std::uint16_t expected_did,
    TextField& target,
    const Step next_step,
    const ecu::core::v2::time::MonotonicClockReading& now) noexcept {
  const auto service_status = uds_.service(now);

  if (uds_.has_response()) {
    const auto response = uds_.take_response();
    last_uds_status_ = response.status;
    last_transport_failure_ = response.transport_failure;
    if (response.status ==
        ecu::core::v2::protocol::uds::UdsStatus::negative_response) {
      return fail(response.negative_response_code);
    }
    if (response.status !=
        ecu::core::v2::protocol::uds::UdsStatus::ok) {
      return fail();
    }
    if (!parse_text_did(response, expected_did, target)) {
      return fail();
    }

    if (next_step == Step::done) {
      step_ = Step::done;
      status_ = IdentificationProgramStatus::complete;
    } else {
      step_ = next_step;
    }
    return ecu::bench::BenchComponentStatus::ok;
  }

  last_uds_status_ = service_status;
  if (!active_uds_status(service_status)) {
    return fail();
  }

  return ecu::bench::BenchComponentStatus::no_action;
}

bool IdentificationProgram::parse_text_did(
    const ecu::core::v2::protocol::uds::UdsResponse& response,
    const std::uint16_t expected_did,
    TextField& target) noexcept {
  using UdsStatus = ecu::core::v2::protocol::uds::UdsStatus;

  // Failure evidence must not reveal VIN/software/hardware text. Only the
  // requested/observed DID, sizes and the first NON-PRINTABLE byte are kept.
  reply_diagnostic_ = {};
  reply_diagnostic_.requested_did = expected_did;
  reply_diagnostic_.response_length = response.length;
  if (response.length >= 3U) {
    reply_diagnostic_.observed_did = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(
             byte_value(response.payload[1U])) << 8U) |
        byte_value(response.payload[2U]));
  }
  if (response.status != UdsStatus::ok ||
      response.length < 3U ||
      byte_value(response.payload[0U]) != 0x62U) {
    reply_diagnostic_.issue =
        IdentificationReplyIssue::invalid_positive_header;
    return false;
  }
  if (reply_diagnostic_.observed_did != expected_did) {
    reply_diagnostic_.issue = IdentificationReplyIssue::unexpected_did;
    return false;
  }

  if (bitrate_ == CanBitrateProfile::k500k &&
      expected_did == kDidVin && response.length == 20U) {
    bool all_ff = true;
    for (std::size_t i = 3U; i < response.length; ++i) {
      if (byte_value(response.payload[i]) != 0xFFU) {
        all_ff = false;
        break;
      }
    }
    if (all_ff) {
      target = {};
      result_.vin_unprogrammed_ff17 = true;
      return true;
    }
  }

  const std::size_t value_length = response.length - 3U;
  if (value_length == 0U ||
      value_length >= TextField::kCapacity) {
    reply_diagnostic_.issue = IdentificationReplyIssue::invalid_text_length;
    return false;
  }

  target = {};
  target.length = value_length;
  for (std::size_t index = 0U;
       index < value_length; ++index) {
    const auto raw = byte_value(response.payload[index + 3U]);
    if (raw < 0x20U || raw > 0x7EU) {
      target = {};
      reply_diagnostic_.issue =
          IdentificationReplyIssue::non_printable_character;
      reply_diagnostic_.invalid_octet_offset = index + 3U;
      reply_diagnostic_.invalid_octet_value = raw;
      return false;
    }
    target.data[index] = static_cast<char>(raw);
  }
  target.data[value_length] = char{0};
  return true;
}

ecu::bench::BenchComponentStatus IdentificationProgram::fail(
    const std::uint8_t nrc) noexcept {
  last_nrc_ = nrc;
  step_ = Step::fault;
  status_ = IdentificationProgramStatus::fault;
  return ecu::bench::BenchComponentStatus::fault;
}

void IdentificationProgram::reset_state() noexcept {
  step_ = Step::idle;
  status_ = IdentificationProgramStatus::idle;
  result_ = {};
  reply_diagnostic_ = {};
  last_nrc_ = 0U;
  last_uds_status_ =
      ecu::core::v2::protocol::uds::UdsStatus::idle;
  last_transport_failure_ =
      ecu::core::v2::protocol::uds::UdsTransportFailure::none;
}

}  // namespace ecu::dut_profiles::daf_sac
