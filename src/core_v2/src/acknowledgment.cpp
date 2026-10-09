#include "ecu/core_v2/protocol/j1939/acknowledgment.hpp"

namespace ecu::core::v2::protocol::j1939 {

bool decode_acknowledgment(
    const transport::CanFrame& frame,
    AcknowledgmentMessage& acknowledgment) noexcept {
  IdentifierFields fields{};
  if (frame.length != 8U ||
      !decode_classic_frame_identifier(frame, fields) ||
      parameter_group_number(fields) != kAcknowledgmentPgn ||
      !is_claimable_address(fields.source_address)) {
    return false;
  }

  std::uint8_t destination = 0U;
  if (!destination_address(fields, destination) ||
      destination != kGlobalAddress) {
    return false;
  }

  const auto control =
      std::to_integer<std::uint8_t>(frame.payload[0U]);
  if (control >
      static_cast<std::uint8_t>(
          AcknowledgmentControl::cannot_respond)) {
    return false;
  }

  if (frame.payload[2U] != std::byte{0xFFU} ||
      frame.payload[3U] != std::byte{0xFFU}) {
    return false;
  }

  const auto request_source =
      std::to_integer<std::uint8_t>(frame.payload[4U]);
  if (!is_valid_source_address(request_source)) {
    return false;
  }

  const auto requested_pgn =
      static_cast<std::uint32_t>(
          std::to_integer<std::uint8_t>(frame.payload[5U])) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[6U]))
       << 8U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[7U]))
       << 16U);
  if (!is_canonical_pgn(requested_pgn)) {
    return false;
  }

  AcknowledgmentMessage decoded{};
  decoded.control =
      static_cast<AcknowledgmentControl>(control);
  decoded.group_function_value =
      std::to_integer<std::uint8_t>(frame.payload[1U]);
  decoded.request_source_address = request_source;
  decoded.response_source_address = fields.source_address;
  decoded.requested_pgn = requested_pgn;
  acknowledgment = decoded;
  return true;
}

}  // namespace ecu::core::v2::protocol::j1939
