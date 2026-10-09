#pragma once

#include "ecu/core_v2/protocol/uds/uds_client.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/dut_profile/runtime.hpp"
#include "ecu/dut_profiles/daf_sac/profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ecu::dut_profiles::daf_sac {

struct TextField {
  static constexpr std::size_t kCapacity = 64U;

  std::array<char, kCapacity> data{};
  std::size_t length{0U};

  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view{data.data(), length};
  }
};

struct IdentificationResult {
  TextField vin{};
  TextField software{};
  TextField hardware{};
  // Distinguishes an unprogrammed VIN sentinel from a valid VIN and a
  // diagnostic failure. Only confirmed for the 500 kbit/s SAC DUT profile.
  bool vin_unprogrammed_ff17{false};
};

// Privacy-safe metadata for a rejected ReadDataByIdentifier response.
// Never stores or exposes VIN, software text or raw UDS payload.
enum class IdentificationReplyIssue : std::uint8_t {
  none,
  invalid_positive_header,
  unexpected_did,
  invalid_text_length,
  non_printable_character,
};

struct IdentificationReplyDiagnostic {
  IdentificationReplyIssue issue{IdentificationReplyIssue::none};
  std::uint16_t requested_did{0U};
  std::uint16_t observed_did{0U};
  std::size_t response_length{0U};
  std::size_t invalid_octet_offset{0U}; // relative to the UDS response PDU
  std::uint8_t invalid_octet_value{0U}; // only emitted for non-printable bytes
};

enum class IdentificationProgramStatus : std::uint8_t {
  idle,
  prepared,
  running,
  complete,
  fault,
};

class IdentificationProgram final
    : public ecu::dut_profile::IDutProfileProgram {
 public:
  IdentificationProgram(
      CanBitrateProfile bitrate,
      ecu::core::v2::protocol::uds::UdsClient& uds,
      const ecu::core::v2::time::IMonotonicClock& clock,
      ecu::bench::BenchComponentExecutionContract execution) noexcept;

  [[nodiscard]] ecu::dut_profile::DutProfileProgramDescriptor
  descriptor() const noexcept override;

  [[nodiscard]] ecu::bench::BenchComponentExecutionContract
  execution_contract() const noexcept override;

  [[nodiscard]] ecu::bench::BenchComponentStatus prepare(
      const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus activate(
      const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus service(
      const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus safe_stop(
      const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept override;
  [[nodiscard]] ecu::bench::BenchComponentStatus stop(
      const ecu::dut_profile::ResolvedDutSessionPlan& plan) noexcept override;

  [[nodiscard]] IdentificationProgramStatus status() const noexcept;
  [[nodiscard]] const IdentificationResult& result() const noexcept;
  [[nodiscard]] IdentificationReplyDiagnostic last_reply_diagnostic()
      const noexcept;
  [[nodiscard]] std::uint8_t last_nrc() const noexcept;
  [[nodiscard]] ecu::core::v2::protocol::uds::UdsStatus
  last_uds_status() const noexcept;
  [[nodiscard]] ecu::core::v2::protocol::uds::UdsTransportFailure
  last_transport_failure() const noexcept;

  ~IdentificationProgram() = default;

 private:
  enum class Step : std::uint8_t {
    idle,
    request_vin,
    wait_vin,
    request_software,
    wait_software,
    request_hardware,
    wait_hardware,
    done,
    fault,
  };

  [[nodiscard]] bool plan_matches(
      const ecu::dut_profile::ResolvedDutSessionPlan& plan) const noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus start_did(
      std::uint16_t did,
      Step wait_step) noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus service_wait(
      std::uint16_t expected_did,
      TextField& target,
      Step next_step,
      const ecu::core::v2::time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] bool parse_text_did(
      const ecu::core::v2::protocol::uds::UdsResponse& response,
      std::uint16_t expected_did,
      TextField& target) noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus fail(
      std::uint8_t nrc = 0U) noexcept;
  void reset_state() noexcept;

  CanBitrateProfile bitrate_;
  ecu::core::v2::protocol::uds::UdsClient& uds_;
  const ecu::core::v2::time::IMonotonicClock& clock_;
  ecu::bench::BenchComponentExecutionContract execution_{};

  Step step_{Step::idle};
  IdentificationProgramStatus status_{IdentificationProgramStatus::idle};
  IdentificationResult result_{};
  IdentificationReplyDiagnostic reply_diagnostic_{};
  std::uint8_t last_nrc_{0U};
  ecu::core::v2::protocol::uds::UdsStatus last_uds_status_{
      ecu::core::v2::protocol::uds::UdsStatus::idle};
  ecu::core::v2::protocol::uds::UdsTransportFailure
      last_transport_failure_{
          ecu::core::v2::protocol::uds::UdsTransportFailure::none};
};

}  // namespace ecu::dut_profiles::daf_sac
