#pragma once

#include "ecu/core_v2/protocol/uds/uds_client.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"
#include "ecu/dut_profiles/daf_sac/identification_program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::dut_profiles::daf_sac {

enum class SacService : std::uint8_t {
  identify,
  read_voltage,
  read_dtcs,
  clear_dtcs,
};

struct SacVoltage {
  bool valid{false};
  float permanent_v{0.0F};
  float ignition_v{0.0F};
};

struct SacDtc {
  std::uint32_t code{0U}; // ISO-14229: three-byte DTC
  std::uint8_t status{0U};
};

struct SacDtcList {
  static constexpr std::size_t kMaxEntries = 128U;
  std::array<SacDtc, kMaxEntries> records{};
  std::size_t count{0U};
  std::uint8_t status_availability{0U};
  std::uint8_t requested_mask{0xFFU};
  bool valid{false};
};

struct SacPressure {
  bool received{false};
  bool pressure1_valid{false};
  bool pressure2_valid{false};
  float pressure1_bar{0.0F};
  float pressure2_bar{0.0F};
};

// Passive J1939 PGN 0xFEAE, ECU source address 0x30.
// RAW 0xFB..0xFF are special/out-of-range values, not pressures.
// This object is a CAN subscriber owned by the app composition.
class PressureMonitor final : public ecu::core::v2::transport::ICanFrameSink {
 public:
  void on_can_frame(
      const ecu::core::v2::transport::ReceivedCanFrame& frame) noexcept override;
  void reset() noexcept;
  [[nodiscard]] SacPressure sample() const noexcept;

 private:
  SacPressure last_{};
};

// One SAC-specific nonblocking operation at a time; lifetime is controlled
// by the common Bench Runtime through DutProfileSessionEndpoint.
// No direct physical driver, threads, heap allocations or OS dependencies.
class ServiceProgram final : public ecu::dut_profile::IDutProfileProgram {
 public:
  ServiceProgram(
      CanBitrateProfile bitrate,
      ecu::core::v2::protocol::uds::UdsClient& uds,
      const ecu::core::v2::time::IMonotonicClock& clock,
      ecu::bench::BenchComponentExecutionContract execution) noexcept;

  [[nodiscard]] bool select(
      SacService service, std::uint8_t dtc_status_mask = 0xFFU) noexcept;
  [[nodiscard]] SacService selected() const noexcept;
  [[nodiscard]] IdentificationProgramStatus status() const noexcept;
  [[nodiscard]] const IdentificationResult& identification() const noexcept;
  [[nodiscard]] SacVoltage voltage() const noexcept;
  [[nodiscard]] const SacDtcList& dtcs() const noexcept;
  [[nodiscard]] bool clear_acknowledged() const noexcept;
  [[nodiscard]] bool clear_request_submitted() const noexcept;

  [[nodiscard]] std::uint8_t last_nrc() const noexcept;
  [[nodiscard]] ecu::core::v2::protocol::uds::UdsStatus last_uds_status()
      const noexcept;
  [[nodiscard]] ecu::core::v2::protocol::uds::UdsTransportFailure
  last_transport_failure() const noexcept;

  [[nodiscard]] ecu::dut_profile::DutProfileProgramDescriptor descriptor()
      const noexcept override;
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

 private:
  enum class Phase : std::uint8_t {
    idle, request_session, wait_session,
    request_data, wait_data, done, fault,
  };
  [[nodiscard]] bool plan_matches(
      const ecu::dut_profile::ResolvedDutSessionPlan& plan) const noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus send_request() noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus handle_response(
      const ecu::core::v2::time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] bool parse_payload(
      const ecu::core::v2::protocol::uds::UdsResponse& response) noexcept;
  [[nodiscard]] ecu::bench::BenchComponentStatus fail(
      std::uint8_t nrc = 0U) noexcept;

  IdentificationProgram identification_;
  ecu::core::v2::protocol::uds::UdsClient& uds_;
  const ecu::core::v2::time::IMonotonicClock& clock_;
  ecu::bench::BenchComponentExecutionContract execution_{};
  ecu::core::v2::protocol::uds::UdsTiming initial_timing_{};
  SacService selected_{SacService::identify};
  std::uint8_t mask_{0xFFU};
  Phase phase_{Phase::idle};
  IdentificationProgramStatus status_{IdentificationProgramStatus::idle};
  SacVoltage voltage_{};
  SacDtcList dtcs_{};
  bool clear_acknowledged_{false};
  bool clear_request_submitted_{false};
  std::uint8_t last_nrc_{0U};
  ecu::core::v2::protocol::uds::UdsStatus last_status_{
      ecu::core::v2::protocol::uds::UdsStatus::idle};
  ecu::core::v2::protocol::uds::UdsTransportFailure last_transport_failure_{
      ecu::core::v2::protocol::uds::UdsTransportFailure::none};
};

}  // namespace ecu::dut_profiles::daf_sac
