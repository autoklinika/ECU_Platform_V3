#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"

namespace ecu::core::v2::protocol::j1939 {

bool decode_identifier(
    const std::uint32_t identifier,
    IdentifierFields& fields) noexcept {
  if (identifier > kMaxCanIdentifier) {
    return false;
  }

  fields.priority =
      static_cast<std::uint8_t>((identifier >> 26U) & 0x07U);
  fields.reserved = ((identifier >> 25U) & 0x01U) != 0U;
  fields.data_page = ((identifier >> 24U) & 0x01U) != 0U;
  fields.pdu_format =
      static_cast<std::uint8_t>((identifier >> 16U) & 0xFFU);
  fields.pdu_specific =
      static_cast<std::uint8_t>((identifier >> 8U) & 0xFFU);
  fields.source_address =
      static_cast<std::uint8_t>(identifier & 0xFFU);

  return !fields.reserved &&
         is_valid_source_address(fields.source_address);
}

bool decode_classic_frame_identifier(
    const transport::CanFrame& frame,
    IdentifierFields& fields) noexcept {
  return frame.identifier_format ==
             transport::CanIdentifierFormat::extended_29_bit &&
         frame.format == transport::CanFrameFormat::classic &&
         frame.type == transport::CanFrameType::data &&
         decode_identifier(frame.identifier, fields);
}

std::uint32_t parameter_group_number(
    const IdentifierFields& fields) noexcept {
  if (fields.reserved) {
    return kMaxPgn + 1U;
  }

  std::uint32_t pgn =
      (static_cast<std::uint32_t>(fields.data_page) << 16U) |
      (static_cast<std::uint32_t>(fields.pdu_format) << 8U);

  if (!is_pdu1(fields)) {
    pgn |= static_cast<std::uint32_t>(fields.pdu_specific);
  }

  return pgn;
}

bool destination_address(
    const IdentifierFields& fields,
    std::uint8_t& destination) noexcept {
  if (fields.reserved || !is_pdu1(fields)) {
    return false;
  }

  destination = fields.pdu_specific;
  return true;
}

bool encode_identifier(
    const MessageAddress& address,
    std::uint32_t& identifier) noexcept {
  if (address.priority > 7U ||
      address.pgn > kMaxPgn ||
      !is_valid_source_address(address.source_address)) {
    return false;
  }

  const auto pdu_format =
      static_cast<std::uint8_t>((address.pgn >> 8U) & 0xFFU);
  std::uint32_t mapped_pgn = address.pgn;

  if (pdu_format < 240U) {
    if ((address.pgn & 0xFFU) != 0U) {
      return false;
    }
    mapped_pgn |=
        static_cast<std::uint32_t>(address.destination_address);
  } else if (address.destination_address != kGlobalAddress) {
    return false;
  }

  identifier =
      (static_cast<std::uint32_t>(address.priority) << 26U) |
      (mapped_pgn << 8U) |
      static_cast<std::uint32_t>(address.source_address);

  return identifier <= kMaxCanIdentifier;
}

bool build_classic_data_frame(
    const MessageAddress& address,
    const std::uint8_t* const payload,
    const std::uint8_t length,
    transport::CanFrame& frame) noexcept {
  if (length > 8U || (length != 0U && payload == nullptr)) {
    return false;
  }

  std::uint32_t identifier = 0U;
  if (!encode_identifier(address, identifier)) {
    return false;
  }

  frame = {};
  frame.identifier = identifier;
  frame.identifier_format =
      transport::CanIdentifierFormat::extended_29_bit;
  frame.format = transport::CanFrameFormat::classic;
  frame.type = transport::CanFrameType::data;
  frame.length = length;

  for (std::uint8_t i = 0U; i < length; ++i) {
    frame.payload[i] = static_cast<std::byte>(payload[i]);
  }

  return true;
}

}  // namespace ecu::core::v2::protocol::j1939
