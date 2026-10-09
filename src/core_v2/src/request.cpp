#include "ecu/core_v2/protocol/j1939/request.hpp"

#include <cstddef>

namespace ecu::core::v2::protocol::j1939 {

bool is_canonical_pgn(const std::uint32_t pgn) noexcept {
  if (pgn > kMaxPgn) {
    return false;
  }

  const auto pdu_format =
      static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU);
  return pdu_format >= 240U || (pgn & 0xFFU) == 0U;
}

bool decode_request(
    const transport::CanFrame& frame,
    RequestMessage& request) noexcept {
  IdentifierFields fields{};
  if (frame.length != 3U ||
      !decode_classic_frame_identifier(frame, fields) ||
      parameter_group_number(fields) != kRequestPgn) {
    return false;
  }

  std::uint8_t destination = 0U;
  if (!destination_address(fields, destination)) {
    return false;
  }

  const auto requested_pgn =
      static_cast<std::uint32_t>(
          std::to_integer<std::uint8_t>(frame.payload[0U])) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[1U]))
       << 8U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[2U]))
       << 16U);

  if (!is_canonical_pgn(requested_pgn)) {
    return false;
  }

  request.source_address = fields.source_address;
  request.destination_address = destination;
  request.requested_pgn = requested_pgn;
  return true;
}

bool build_request(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint32_t requested_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_valid_source_address(source_address) ||
      !is_canonical_pgn(requested_pgn)) {
    return false;
  }

  const std::uint8_t payload[3] = {
      static_cast<std::uint8_t>(requested_pgn & 0xFFU),
      static_cast<std::uint8_t>((requested_pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((requested_pgn >> 16U) & 0xFFU)};

  return build_classic_data_frame(
      MessageAddress{
          6U,
          kRequestPgn,
          source_address,
          destination_address},
      payload,
      3U,
      frame);
}

}  // namespace ecu::core::v2::protocol::j1939
