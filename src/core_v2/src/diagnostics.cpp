#include "ecu/core_v2/protocol/j1939/diagnostics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {
namespace {

[[nodiscard]] DiagnosticLampStatus lamp_status(
    const std::uint8_t value,
    const std::uint8_t shift) noexcept {
  return static_cast<DiagnosticLampStatus>(
      (value >> shift) & 0x03U);
}

[[nodiscard]] DiagnosticLampFlash lamp_flash(
    const std::uint8_t value,
    const std::uint8_t shift) noexcept {
  return static_cast<DiagnosticLampFlash>(
      (value >> shift) & 0x03U);
}

[[nodiscard]] bool single_frame_dtc_absent(
    const transport::CanFrame& frame) noexcept {
  return frame.payload[2U] == std::byte{0U} &&
         frame.payload[3U] == std::byte{0U} &&
         frame.payload[4U] == std::byte{0U} &&
         frame.payload[5U] == std::byte{0U};
}

[[nodiscard]] bool single_frame_padding_valid(
    const transport::CanFrame& frame) noexcept {
  return frame.payload[6U] == std::byte{0xFFU} &&
         frame.payload[7U] == std::byte{0xFFU};
}

[[nodiscard]] DiagnosticDecodeStatus decode_dtc_bytes(
    const std::byte* const bytes,
    DiagnosticTroubleCode& dtc) noexcept {
  if (bytes == nullptr) {
    return DiagnosticDecodeStatus::invalid_argument;
  }

  const auto b0 = std::to_integer<std::uint8_t>(bytes[0U]);
  const auto b1 = std::to_integer<std::uint8_t>(bytes[1U]);
  const auto b2 = std::to_integer<std::uint8_t>(bytes[2U]);
  const auto b3 = std::to_integer<std::uint8_t>(bytes[3U]);

  dtc = {};
  dtc.fmi = static_cast<std::uint8_t>(b2 & 0x1FU);
  dtc.occurrence_count = static_cast<std::uint8_t>(b3 & 0x7FU);
  dtc.conversion_method = (b3 & 0x80U) != 0U;

  if (dtc.conversion_method) {
    return DiagnosticDecodeStatus::unsupported_conversion_method;
  }

  dtc.spn =
      static_cast<std::uint32_t>(b0) |
      (static_cast<std::uint32_t>(b1) << 8U) |
      (static_cast<std::uint32_t>(b2 & 0xE0U) << 11U);
  return DiagnosticDecodeStatus::ok;
}

[[nodiscard]] bool decode_frame_identifier(
    const transport::CanFrame& frame,
    const std::uint32_t expected_pgn,
    std::uint8_t& source_address) noexcept {
  IdentifierFields fields{};
  if (!decode_classic_frame_identifier(frame, fields) ||
      parameter_group_number(fields) != expected_pgn ||
      !is_claimable_address(fields.source_address)) {
    return false;
  }

  source_address = fields.source_address;
  return true;
}

[[nodiscard]] bool decode_dtc_list_identifier(
    const transport::CanFrame& frame,
    std::uint32_t& pgn,
    std::uint8_t& source_address) noexcept {
  IdentifierFields fields{};
  if (!decode_classic_frame_identifier(frame, fields)) {
    return false;
  }

  pgn = parameter_group_number(fields);
  source_address = fields.source_address;
  return is_dtc_list_pgn(pgn) &&
         is_claimable_address(source_address);
}

[[nodiscard]] bool valid_transport_delivery(
    const TpMessage& message) noexcept {
  if (!is_claimable_address(message.source_address) ||
      message.source_address == message.destination_address) {
    return false;
  }

  if (message.broadcast) {
    return message.destination_address == kGlobalAddress;
  }

  return is_claimable_address(message.destination_address);
}

[[nodiscard]] bool scan_dm4_records(
    const TpMessage& message,
    std::size_t& record_count) noexcept {
  record_count = 0U;
  if (message.size == 0U ||
      static_cast<std::size_t>(message.size) >
          message.data.size()) {
    return false;
  }

  std::size_t offset = 0U;
  const auto size = static_cast<std::size_t>(message.size);

  while (offset < size) {
    const auto freeze_frame_length =
        std::to_integer<std::uint8_t>(message.data[offset]);
    if (freeze_frame_length < kDm4StandardPayloadBytes) {
      return false;
    }

    const auto total_record_bytes =
        static_cast<std::size_t>(freeze_frame_length) + 1U;
    if (total_record_bytes > size - offset) {
      return false;
    }

    offset += total_record_bytes;
    ++record_count;
  }

  return offset == size && record_count != 0U;
}

[[nodiscard]] bool locate_dm4_record(
    const TpMessage& message,
    const std::size_t requested_index,
    std::size_t& record_offset,
    std::size_t& record_payload_length) noexcept {
  std::size_t count = 0U;
  if (!scan_dm4_records(message, count) ||
      requested_index >= count) {
    return false;
  }

  std::size_t offset = 0U;
  for (std::size_t index = 0U;
       index <= requested_index;
       ++index) {
    const auto length =
        static_cast<std::size_t>(
            std::to_integer<std::uint8_t>(
                message.data[offset]));
    if (index == requested_index) {
      record_offset = offset;
      record_payload_length = length;
      return true;
    }
    offset += length + 1U;
  }

  return false;
}

}  // namespace

bool decode_diagnostic_lamps(
    const std::byte lamp_status_byte,
    const std::byte lamp_flash_byte,
    DiagnosticLampState& state) noexcept {
  const auto status =
      std::to_integer<std::uint8_t>(lamp_status_byte);
  const auto flash =
      std::to_integer<std::uint8_t>(lamp_flash_byte);

  state.malfunction_indicator = lamp_status(status, 6U);
  state.red_stop = lamp_status(status, 4U);
  state.amber_warning = lamp_status(status, 2U);
  state.protect = lamp_status(status, 0U);

  state.malfunction_indicator_flash = lamp_flash(flash, 6U);
  state.red_stop_flash = lamp_flash(flash, 4U);
  state.amber_warning_flash = lamp_flash(flash, 2U);
  state.protect_flash = lamp_flash(flash, 0U);
  return true;
}

bool decode_dm_frame(
    const transport::CanFrame& frame,
    DiagnosticMessageInfo& info) noexcept {
  std::uint32_t pgn = 0U;
  std::uint8_t source_address = kNullAddress;
  if (frame.length != 8U ||
      !decode_dtc_list_identifier(
          frame,
          pgn,
          source_address) ||
      !single_frame_padding_valid(frame)) {
    return false;
  }

  DiagnosticMessageInfo decoded{};
  decoded.pgn = pgn;
  decoded.source_address = source_address;
  decoded.transported = false;
  decoded.destination_specific = false;
  decoded.destination_address = kGlobalAddress;
  decoded.dtc_count =
      single_frame_dtc_absent(frame) ? 0U : 1U;
  static_cast<void>(
      decode_diagnostic_lamps(
          frame.payload[0U],
          frame.payload[1U],
          decoded.lamps));

  info = decoded;
  return true;
}

bool decode_dm_transport(
    const TpMessage& message,
    DiagnosticMessageInfo& info) noexcept {
  if (!is_dtc_list_pgn(message.pgn) ||
      !valid_transport_delivery(message) ||
      message.size < 10U ||
      static_cast<std::size_t>(message.size) >
          message.data.size() ||
      ((static_cast<std::size_t>(message.size) - 2U) %
       kDtcEncodedBytes) != 0U) {
    return false;
  }

  DiagnosticMessageInfo decoded{};
  decoded.pgn = message.pgn;
  decoded.source_address = message.source_address;
  decoded.transported = true;
  decoded.destination_specific = !message.broadcast;
  decoded.destination_address =
      message.destination_address;
  decoded.dtc_count =
      (static_cast<std::size_t>(message.size) - 2U) /
      kDtcEncodedBytes;
  static_cast<void>(
      decode_diagnostic_lamps(
          message.data[0U],
          message.data[1U],
          decoded.lamps));

  info = decoded;
  return true;
}

DiagnosticDecodeStatus decode_dtc(
    const transport::CanFrame& frame,
    const std::size_t index,
    DiagnosticTroubleCode& dtc) noexcept {
  DiagnosticMessageInfo info{};
  if (!decode_dm_frame(frame, info) ||
      index >= info.dtc_count) {
    return DiagnosticDecodeStatus::invalid_argument;
  }

  const auto offset = 2U + (index * kDtcEncodedBytes);
  return decode_dtc_bytes(
      frame.payload.data() + offset,
      dtc);
}

DiagnosticDecodeStatus decode_dtc(
    const TpMessage& message,
    const std::size_t index,
    DiagnosticTroubleCode& dtc) noexcept {
  DiagnosticMessageInfo info{};
  if (!decode_dm_transport(message, info) ||
      index >= info.dtc_count) {
    return DiagnosticDecodeStatus::invalid_argument;
  }

  const auto offset = 2U + (index * kDtcEncodedBytes);
  return decode_dtc_bytes(
      message.data.data() + offset,
      dtc);
}

bool decode_dm4_frame(
    const transport::CanFrame& frame,
    DiagnosticFreezeFrameInfo& info) noexcept {
  std::uint8_t source_address = kNullAddress;
  if (frame.length != 8U ||
      !decode_frame_identifier(
          frame,
          kDm4Pgn,
          source_address) ||
      frame.payload[0U] != std::byte{0U} ||
      frame.payload[1U] != std::byte{0U} ||
      frame.payload[2U] != std::byte{0U} ||
      frame.payload[3U] != std::byte{0U} ||
      frame.payload[4U] != std::byte{0U} ||
      frame.payload[5U] != std::byte{0xFFU} ||
      frame.payload[6U] != std::byte{0xFFU} ||
      frame.payload[7U] != std::byte{0xFFU}) {
    return false;
  }

  DiagnosticFreezeFrameInfo decoded{};
  decoded.source_address = source_address;
  decoded.record_count = 0U;
  decoded.transported = false;
  decoded.destination_specific = false;
  decoded.destination_address = kGlobalAddress;
  decoded.no_dtc = true;
  info = decoded;
  return true;
}

bool decode_dm4_transport(
    const TpMessage& message,
    DiagnosticFreezeFrameInfo& info) noexcept {
  std::size_t record_count = 0U;
  if (message.pgn != kDm4Pgn ||
      !valid_transport_delivery(message) ||
      !scan_dm4_records(message, record_count)) {
    return false;
  }

  DiagnosticFreezeFrameInfo decoded{};
  decoded.source_address = message.source_address;
  decoded.record_count = record_count;
  decoded.transported = true;
  decoded.destination_specific = !message.broadcast;
  decoded.destination_address =
      message.destination_address;
  decoded.no_dtc = false;
  info = decoded;
  return true;
}

DiagnosticDecodeStatus decode_dm4_record(
    const TpMessage& message,
    const std::size_t index,
    DiagnosticFreezeFrameRecord& record) noexcept {
  DiagnosticFreezeFrameInfo info{};
  if (!decode_dm4_transport(message, info) ||
      index >= info.record_count) {
    return DiagnosticDecodeStatus::invalid_argument;
  }

  std::size_t offset = 0U;
  std::size_t payload_length = 0U;
  if (!locate_dm4_record(
          message,
          index,
          offset,
          payload_length) ||
      payload_length < kDm4StandardPayloadBytes) {
    return DiagnosticDecodeStatus::invalid_argument;
  }

  DiagnosticFreezeFrameRecord decoded{};
  const auto dtc_status =
      decode_dtc_bytes(
          message.data.data() + offset + 1U,
          decoded.dtc);

  decoded.engine_torque_mode_raw =
      std::to_integer<std::uint8_t>(
          message.data[offset + 5U]);
  decoded.boost_pressure_raw =
      std::to_integer<std::uint8_t>(
          message.data[offset + 6U]);
  decoded.engine_speed_raw =
      static_cast<std::uint16_t>(
          std::to_integer<std::uint8_t>(
              message.data[offset + 7U])) |
      static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(
              std::to_integer<std::uint8_t>(
                  message.data[offset + 8U]))
          << 8U);
  decoded.engine_load_raw =
      std::to_integer<std::uint8_t>(
          message.data[offset + 9U]);
  decoded.coolant_temperature_raw =
      std::to_integer<std::uint8_t>(
          message.data[offset + 10U]);
  decoded.vehicle_speed_raw =
      static_cast<std::uint16_t>(
          std::to_integer<std::uint8_t>(
              message.data[offset + 11U])) |
      static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(
              std::to_integer<std::uint8_t>(
                  message.data[offset + 12U]))
          << 8U);

  decoded.manufacturer_data_offset =
      offset + 13U;
  decoded.manufacturer_data_length =
      payload_length - kDm4StandardPayloadBytes;

  record = decoded;
  return dtc_status;
}

bool decode_dm5_frame(
    const transport::CanFrame& frame,
    DiagnosticReadiness1& readiness) noexcept {
  std::uint8_t source_address = kNullAddress;
  if (frame.length != 8U ||
      !decode_frame_identifier(
          frame,
          kDm5Pgn,
          source_address)) {
    return false;
  }

  DiagnosticReadiness1 decoded{};
  decoded.source_address = source_address;
  decoded.active_dtc_count =
      std::to_integer<std::uint8_t>(
          frame.payload[0U]);
  decoded.previously_active_dtc_count =
      std::to_integer<std::uint8_t>(
          frame.payload[1U]);
  decoded.obd_compliance =
      std::to_integer<std::uint8_t>(
          frame.payload[2U]);
  decoded.continuously_monitored_support_status =
      std::to_integer<std::uint8_t>(
          frame.payload[3U]);
  decoded.noncontinuously_monitored_support_byte5 =
      std::to_integer<std::uint8_t>(
          frame.payload[4U]);
  decoded.noncontinuously_monitored_support_byte6 =
      std::to_integer<std::uint8_t>(
          frame.payload[5U]);
  decoded.noncontinuously_monitored_status_byte7 =
      std::to_integer<std::uint8_t>(
          frame.payload[6U]);
  decoded.noncontinuously_monitored_status_byte8 =
      std::to_integer<std::uint8_t>(
          frame.payload[7U]);

  readiness = decoded;
  return true;
}

}  // namespace ecu::core::v2::protocol::j1939
