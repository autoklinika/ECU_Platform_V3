#pragma once

#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"
#include "ecu/core_v2/protocol/j1939/transport_protocol.hpp"

#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

inline constexpr std::uint32_t kDm1Pgn = 0xFECAU;
inline constexpr std::uint32_t kDm2Pgn = 0xFECBU;
inline constexpr std::uint32_t kDm4Pgn = 0xFECDU;
inline constexpr std::uint32_t kDm5Pgn = 0xFECEU;
inline constexpr std::uint32_t kDm6Pgn = 0xFECFU;
inline constexpr std::uint32_t kDm12Pgn = 0xFED4U;

inline constexpr std::size_t kDtcEncodedBytes = 4U;
inline constexpr std::size_t kDm4StandardPayloadBytes = 12U;

enum class DiagnosticLampStatus : std::uint8_t {
  off = 0U,
  on = 1U,
  error = 2U,
  not_available = 3U,
};

enum class DiagnosticLampFlash : std::uint8_t {
  slow = 0U,
  fast = 1U,
  reserved = 2U,
  not_available = 3U,
};

struct DiagnosticLampState {
  DiagnosticLampStatus malfunction_indicator{
      DiagnosticLampStatus::not_available};
  DiagnosticLampStatus red_stop{
      DiagnosticLampStatus::not_available};
  DiagnosticLampStatus amber_warning{
      DiagnosticLampStatus::not_available};
  DiagnosticLampStatus protect{
      DiagnosticLampStatus::not_available};

  DiagnosticLampFlash malfunction_indicator_flash{
      DiagnosticLampFlash::not_available};
  DiagnosticLampFlash red_stop_flash{
      DiagnosticLampFlash::not_available};
  DiagnosticLampFlash amber_warning_flash{
      DiagnosticLampFlash::not_available};
  DiagnosticLampFlash protect_flash{
      DiagnosticLampFlash::not_available};
};

struct DiagnosticTroubleCode {
  std::uint32_t spn{0U};
  std::uint8_t fmi{0U};
  std::uint8_t occurrence_count{0U};
  bool conversion_method{false};
};

enum class DiagnosticDecodeStatus : std::uint8_t {
  ok,
  invalid_argument,
  unsupported_conversion_method,
};

struct DiagnosticMessageInfo {
  std::uint32_t pgn{0U};
  std::uint8_t source_address{kNullAddress};
  DiagnosticLampState lamps{};
  std::size_t dtc_count{0U};
  bool transported{false};
  bool destination_specific{false};
  std::uint8_t destination_address{kGlobalAddress};
};

struct DiagnosticReadiness1 {
  std::uint8_t source_address{kNullAddress};
  std::uint8_t active_dtc_count{0U};
  std::uint8_t previously_active_dtc_count{0U};
  std::uint8_t obd_compliance{0U};
  std::uint8_t continuously_monitored_support_status{0U};
  std::uint8_t noncontinuously_monitored_support_byte5{0U};
  std::uint8_t noncontinuously_monitored_support_byte6{0U};
  std::uint8_t noncontinuously_monitored_status_byte7{0U};
  std::uint8_t noncontinuously_monitored_status_byte8{0U};
};

struct DiagnosticFreezeFrameInfo {
  std::uint8_t source_address{kNullAddress};
  std::size_t record_count{0U};
  bool transported{false};
  bool destination_specific{false};
  std::uint8_t destination_address{kGlobalAddress};
  bool no_dtc{false};
};

struct DiagnosticFreezeFrameRecord {
  DiagnosticTroubleCode dtc{};

  // Raw J1939-71 encoded snapshot values. Core V2 deliberately does not
  // apply J1939-71 engineering-unit scaling in the J1939-73 parser.
  std::uint8_t engine_torque_mode_raw{0U};
  std::uint8_t boost_pressure_raw{0U};
  std::uint16_t engine_speed_raw{0U};
  std::uint8_t engine_load_raw{0U};
  std::uint8_t coolant_temperature_raw{0U};
  std::uint16_t vehicle_speed_raw{0U};

  std::size_t manufacturer_data_offset{0U};
  std::size_t manufacturer_data_length{0U};
};

[[nodiscard]] constexpr bool is_dm1_or_dm2_pgn(
    const std::uint32_t pgn) noexcept {
  return pgn == kDm1Pgn || pgn == kDm2Pgn;
}

[[nodiscard]] constexpr bool is_dtc_list_pgn(
    const std::uint32_t pgn) noexcept {
  return pgn == kDm1Pgn ||
         pgn == kDm2Pgn ||
         pgn == kDm6Pgn ||
         pgn == kDm12Pgn;
}

[[nodiscard]] bool decode_diagnostic_lamps(
    std::byte lamp_status,
    std::byte lamp_flash,
    DiagnosticLampState& state) noexcept;

// Read-only DTC-list parser. Supports DM1, DM2, DM6 and DM12.
[[nodiscard]] bool decode_dm_frame(
    const transport::CanFrame& frame,
    DiagnosticMessageInfo& info) noexcept;

[[nodiscard]] bool decode_dm_transport(
    const TpMessage& message,
    DiagnosticMessageInfo& info) noexcept;

[[nodiscard]] DiagnosticDecodeStatus decode_dtc(
    const transport::CanFrame& frame,
    std::size_t index,
    DiagnosticTroubleCode& dtc) noexcept;

[[nodiscard]] DiagnosticDecodeStatus decode_dtc(
    const TpMessage& message,
    std::size_t index,
    DiagnosticTroubleCode& dtc) noexcept;

// DM4 Freeze Frame Parameters. A no-DTC response fits in one classic frame;
// actual freeze-frame records require a reassembled TP message.
[[nodiscard]] bool decode_dm4_frame(
    const transport::CanFrame& frame,
    DiagnosticFreezeFrameInfo& info) noexcept;

[[nodiscard]] bool decode_dm4_transport(
    const TpMessage& message,
    DiagnosticFreezeFrameInfo& info) noexcept;

[[nodiscard]] DiagnosticDecodeStatus decode_dm4_record(
    const TpMessage& message,
    std::size_t index,
    DiagnosticFreezeFrameRecord& record) noexcept;

// DM5 Diagnostic Readiness 1 is a fixed 8-byte read-only response.
[[nodiscard]] bool decode_dm5_frame(
    const transport::CanFrame& frame,
    DiagnosticReadiness1& readiness) noexcept;

}  // namespace ecu::core::v2::protocol::j1939
