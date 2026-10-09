#include "ecu/dut_profiles/daf_sac/service_program.hpp"

#include "ecu/core_v2/protocol/uds/uds_services.hpp"

#include <chrono>

namespace ecu::dut_profiles::daf_sac {
namespace {
namespace uds = ecu::core::v2::protocol::uds;
namespace core = ecu::core::v2;
namespace bench = ecu::bench;

[[nodiscard]] std::uint8_t raw(const std::byte b) noexcept {
  return std::to_integer<std::uint8_t>(b);
}

[[nodiscard]] bool active(const uds::UdsStatus status) noexcept {
  return status == uds::UdsStatus::ok ||
         status == uds::UdsStatus::idle ||
         status == uds::UdsStatus::in_progress;
}

// SAC-specific client-side latency allowance. The ECU advertises P2=25 ms
// in 50 03; losing a scheduling slot after ClearDiagnosticInformation must
// not silently turn a reply into a false timeout. This does NOT modify the
// ECU-reported timings or the transport-neutral UDS/Core contracts.
[[nodiscard]] bool ensure_client_wait(
    uds::UdsClient& client,
    const std::chrono::milliseconds minimum_p2,
    const std::chrono::milliseconds minimum_p2_star) noexcept {
  auto timing = client.timing();
  if (timing.p2 < minimum_p2) {
    timing.p2 = minimum_p2;
  }
  if (timing.p2_star < minimum_p2_star) {
    timing.p2_star = minimum_p2_star;
  }
  client.set_timing(timing);
  return client.timing().p2 == timing.p2 &&
         client.timing().p2_star == timing.p2_star;
}

[[nodiscard]] std::uint16_t big16(
    const std::byte high, const std::byte low) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(raw(high)) << 8U) | raw(low));
}

[[nodiscard]] bool parse_voltage(
    const uds::UdsResponse& response,
    SacVoltage& destination) noexcept {
  // Legacy FE96: positive SID/DID followed by four data octets;
  // supply at 7/8, ignition at 9/10, both big-endian with 0.1 V/LSB.
  if (response.status != uds::UdsStatus::ok ||
      response.length < 11U ||
      raw(response.payload[0U]) != 0x62U ||
      raw(response.payload[1U]) != 0xFEU ||
      raw(response.payload[2U]) != 0x96U) {
    return false;
  }
  const auto permanent = big16(
      response.payload[7U], response.payload[8U]);
  const auto ignition = big16(
      response.payload[9U], response.payload[10U]);
  // SAC is a low-voltage truck controller; reject unsupported readings
  // above 60V, including OEM sentinel patterns FEFE/FFFF.
  if (permanent > 600U || ignition > 600U) {
    return false;
  }
  destination = {true,
                 static_cast<float>(permanent) / 10.0F,
                 static_cast<float>(ignition) / 10.0F};
  return true;
}

[[nodiscard]] bool parse_dtcs(
    const uds::UdsResponse& response,
    const std::uint8_t requested_mask,
    SacDtcList& destination) noexcept {
  if (response.status != uds::UdsStatus::ok ||
      response.length < 3U ||
      raw(response.payload[0U]) != 0x59U ||
      raw(response.payload[1U]) != 0x02U) {
    return false;
  }
  const std::size_t size = response.length - 3U;
  if (size % 4U != 0U || size / 4U > SacDtcList::kMaxEntries) {
    return false;
  }
  SacDtcList next{};
  next.requested_mask = requested_mask;
  next.status_availability = raw(response.payload[2U]);
  next.count = size / 4U;
  next.valid = true;
  for (std::size_t i = 0U; i < next.count; ++i) {
    const std::size_t offset = 3U + 4U * i;
    next.records[i] = {
        (static_cast<std::uint32_t>(raw(response.payload[offset])) << 16U) |
            (static_cast<std::uint32_t>(raw(response.payload[offset + 1U])) << 8U) |
            static_cast<std::uint32_t>(raw(response.payload[offset + 2U])),
        raw(response.payload[offset + 3U])};
  }
  destination = next;
  return true;
}

}  // namespace

void PressureMonitor::on_can_frame(
    const core::transport::ReceivedCanFrame& received) noexcept {
  const auto& frame = received.frame;
  if (frame.identifier_format !=
          core::transport::CanIdentifierFormat::extended_29_bit ||
      frame.format != core::transport::CanFrameFormat::classic ||
      frame.type != core::transport::CanFrameType::data ||
      frame.length != 8U ||
      (frame.identifier & kPressureCanMask) !=
          (kPressureCanId & kPressureCanMask)) {
    return;
  }
  // Keep presence information even if ECU marks both pressure fields invalid.
  const auto p1 = raw(frame.payload[2U]);
  const auto p2 = raw(frame.payload[3U]);
  last_.received = true;
  last_.pressure1_valid = p1 <= 0xFAU;
  last_.pressure2_valid = p2 <= 0xFAU;
  last_.pressure1_bar = last_.pressure1_valid
                            ? static_cast<float>(p1) * 0.08F : 0.0F;
  last_.pressure2_bar = last_.pressure2_valid
                            ? static_cast<float>(p2) * 0.08F : 0.0F;
}

void PressureMonitor::reset() noexcept {
  last_ = {};
}

SacPressure PressureMonitor::sample() const noexcept {
  return last_;
}

ServiceProgram::ServiceProgram(
    const CanBitrateProfile bitrate,
    uds::UdsClient& uds_client,
    const core::time::IMonotonicClock& clock,
    const bench::BenchComponentExecutionContract execution) noexcept
    : identification_(bitrate, uds_client, clock, execution),
      uds_(uds_client), clock_(clock), execution_(execution),
      initial_timing_(uds_client.timing()) {}

bool ServiceProgram::select(
    const SacService service, const std::uint8_t mask) noexcept {
  if (status_ != IdentificationProgramStatus::idle ||
      (service != SacService::identify &&
       service != SacService::read_voltage &&
       service != SacService::read_dtcs &&
       service != SacService::clear_dtcs)) {
    return false;
  }
  selected_ = service;
  mask_ = mask;
  voltage_ = {};
  dtcs_ = {};
  clear_acknowledged_ = false;
  clear_request_submitted_ = false;
  last_nrc_ = 0U;
  last_status_ = uds::UdsStatus::idle;
  last_transport_failure_ = uds::UdsTransportFailure::none;
  return true;
}

SacService ServiceProgram::selected() const noexcept { return selected_; }
IdentificationProgramStatus ServiceProgram::status() const noexcept {
  return selected_ == SacService::identify ? identification_.status() : status_;
}
const IdentificationResult& ServiceProgram::identification() const noexcept {
  return identification_.result();
}
SacVoltage ServiceProgram::voltage() const noexcept { return voltage_; }
const SacDtcList& ServiceProgram::dtcs() const noexcept { return dtcs_; }
bool ServiceProgram::clear_acknowledged() const noexcept { return clear_acknowledged_; }
bool ServiceProgram::clear_request_submitted() const noexcept {
  return clear_request_submitted_;
}

std::uint8_t ServiceProgram::last_nrc() const noexcept {
  return selected_ == SacService::identify
             ? identification_.last_nrc() : last_nrc_;
}
uds::UdsStatus ServiceProgram::last_uds_status() const noexcept {
  return selected_ == SacService::identify
             ? identification_.last_uds_status() : last_status_;
}
uds::UdsTransportFailure ServiceProgram::last_transport_failure()
    const noexcept {
  return selected_ == SacService::identify
             ? identification_.last_transport_failure() : last_transport_failure_;
}

ecu::dut_profile::DutProfileProgramDescriptor
ServiceProgram::descriptor() const noexcept {
  return identification_.descriptor();
}
bench::BenchComponentExecutionContract
ServiceProgram::execution_contract() const noexcept {
  return execution_;
}

bool ServiceProgram::plan_matches(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) const noexcept {
  const auto descriptor = identification_.descriptor();
  return plan.schema_version == descriptor.schema_version &&
         plan.profile_revision == descriptor.profile_revision &&
         plan.profile_id == descriptor.profile_id &&
         plan.can_link_count == 1U &&
         plan.rx_expectation_count == 2U &&
         plan.rx_expectations[1U].identifier == kPressureCanId &&
         plan.rx_expectations[1U].mask == kPressureCanMask &&
         plan.rx_expectations[1U].match_extended &&
         !plan.rx_expectations[1U].match_standard &&
         plan.can_links[0U].channel_config.timestamp_domain ==
             clock_.properties().domain;
}

bench::BenchComponentStatus ServiceProgram::prepare(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (status_ != IdentificationProgramStatus::idle || !plan_matches(plan) ||
      !uds_.valid() ||
      identification_.prepare(plan) != bench::BenchComponentStatus::ok) {
    return fail();
  }
  uds_.reset();
  // A previous clear-specific client timeout must never leak into the
  // next read/identification session.
  uds_.set_timing(initial_timing_);
  if (selected_ == SacService::read_dtcs &&
      !ensure_client_wait(
          uds_, std::chrono::milliseconds{1000},
          std::chrono::milliseconds{5000})) {
    return fail();
  }
  phase_ = Phase::idle;
  status_ = IdentificationProgramStatus::prepared;
  last_status_ = uds::UdsStatus::idle;
  last_nrc_ = 0U;
  last_transport_failure_ = uds::UdsTransportFailure::none;
  return bench::BenchComponentStatus::ok;
}

bench::BenchComponentStatus ServiceProgram::activate(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan) ||
      status_ != IdentificationProgramStatus::prepared) {
    return fail();
  }
  if (selected_ == SacService::identify &&
      identification_.activate(plan) != bench::BenchComponentStatus::ok) {
    return fail();
  }
  phase_ = (selected_ == SacService::read_dtcs ||
            selected_ == SacService::clear_dtcs)
               ? Phase::request_session : Phase::request_data;
  status_ = IdentificationProgramStatus::running;
  return bench::BenchComponentStatus::ok;
}

bench::BenchComponentStatus ServiceProgram::send_request() noexcept {
  uds::UdsRequest request{};
  if (phase_ == Phase::request_session) {
    request = uds::make_diagnostic_session_control(0x03U);
    phase_ = Phase::wait_session;
  } else if (phase_ == Phase::request_data) {
    switch (selected_) {
      case SacService::read_voltage:
        request = uds::make_read_data_by_identifier(kDidVoltage);
        break;
      case SacService::read_dtcs:
        request = uds::make_read_dtc_information_by_status_mask(mask_);
        break;
      case SacService::clear_dtcs:
        // ISO-14229 ClearDiagnosticInformation, all DTC groups.
        // Called ONLY via separately confirmed Application action.
        request.length = 4U;
        request.payload[0U] = std::byte{0x14U};
        request.payload[1U] = std::byte{0xFFU};
        request.payload[2U] = std::byte{0xFFU};
        request.payload[3U] = std::byte{0xFFU};
        break;
      case SacService::identify:
        return fail();
    }
    phase_ = Phase::wait_data;
  } else {
    return fail();
  }
  last_status_ = uds_.start_request(request);
  if (last_status_ != uds::UdsStatus::in_progress) {
    return fail();
  }
  if (selected_ == SacService::clear_dtcs && phase_ == Phase::wait_data) {
    clear_request_submitted_ = true;
  }
  return bench::BenchComponentStatus::ok;
}

bool ServiceProgram::parse_payload(
    const uds::UdsResponse& response) noexcept {
  if (selected_ == SacService::read_voltage) {
    return parse_voltage(response, voltage_);
  }
  if (selected_ == SacService::read_dtcs) {
    return parse_dtcs(response, mask_, dtcs_);
  }
  if (selected_ == SacService::clear_dtcs) {
    if (response.status != uds::UdsStatus::ok ||
        response.length != 1U ||
        raw(response.payload[0U]) != 0x54U) {
      return false;
    }
    clear_acknowledged_ = true; // not proof of zero remaining DTC
    return true;
  }
  return false;
}

bench::BenchComponentStatus ServiceProgram::handle_response(
    const core::time::MonotonicClockReading& now) noexcept {
  const auto serviced = uds_.service(now);
  if (uds_.has_response()) {
    const auto response = uds_.take_response();
    last_status_ = response.status;
    last_transport_failure_ = response.transport_failure;
    if (response.status == uds::UdsStatus::negative_response) {
      return fail(response.negative_response_code);
    }
    if (response.status != uds::UdsStatus::ok) {
      return fail();
    }
    if (phase_ == Phase::wait_session) {
      if (response.length < 2U ||
          raw(response.payload[0U]) != 0x50U ||
          raw(response.payload[1U]) != 0x03U) {
        return fail();
      }
      if (selected_ == SacService::read_dtcs &&
          !ensure_client_wait(
              uds_, std::chrono::milliseconds{1000},
              std::chrono::milliseconds{5000})) {
        // ECU's 50 03 timing bytes remain evidence, but do not shorten
        // the SAC application client's user-space response allowance.
        return fail();
      }
      if (selected_ == SacService::clear_dtcs &&
          !ensure_client_wait(
              uds_, std::chrono::milliseconds{3000},
              std::chrono::milliseconds{5000})) {
        // The destructive request can take longer; no automatic retry.
        return fail();
      }
      phase_ = Phase::request_data;
      return bench::BenchComponentStatus::ok;
    }
    if (phase_ == Phase::wait_data && parse_payload(response)) {
      phase_ = Phase::done;
      status_ = IdentificationProgramStatus::complete;
      return bench::BenchComponentStatus::ok;
    }
    return fail();
  }
  last_status_ = serviced;
  return active(serviced) ? bench::BenchComponentStatus::no_action : fail();
}

bench::BenchComponentStatus ServiceProgram::service(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan) ||
      (status_ != IdentificationProgramStatus::running &&
       status_ != IdentificationProgramStatus::complete)) {
    return fail();
  }
  if (selected_ == SacService::identify) {
    const auto result = identification_.service(plan);
    if (result == bench::BenchComponentStatus::fault) {
      return fail(identification_.last_nrc());
    }
    status_ = identification_.status();
    return result;
  }
  if (status_ == IdentificationProgramStatus::complete) {
    return bench::BenchComponentStatus::no_action;
  }
  const auto now = clock_.read();
  if (!core::time::is_valid_clock_reading(
          now, plan.can_links[0U].channel_config.timestamp_domain)) {
    return fail();
  }
  if (phase_ == Phase::request_session || phase_ == Phase::request_data) {
    return send_request();
  }
  if (phase_ == Phase::wait_session || phase_ == Phase::wait_data) {
    return handle_response(now);
  }
  return fail();
}

bench::BenchComponentStatus ServiceProgram::safe_stop(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan)) {
    return fail();
  }
  const auto cleaned = identification_.safe_stop(plan);
  uds_.reset();
  uds_.set_timing(initial_timing_);
  phase_ = Phase::idle;
  return cleaned;
}

bench::BenchComponentStatus ServiceProgram::stop(
    const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept {
  if (!plan_matches(plan)) {
    return fail();
  }
  const auto cleaned = identification_.stop(plan);
  uds_.reset();
  uds_.set_timing(initial_timing_);
  phase_ = Phase::idle;
  status_ = IdentificationProgramStatus::idle;
  return cleaned;
}

bench::BenchComponentStatus ServiceProgram::fail(const std::uint8_t nrc) noexcept {
  last_nrc_ = nrc;
  phase_ = Phase::fault;
  status_ = IdentificationProgramStatus::fault;
  return bench::BenchComponentStatus::fault;
}

}  // namespace ecu::dut_profiles::daf_sac
