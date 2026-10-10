#include "ecu/core_v2/protocol/j1939/fd_contained_pg.hpp"

namespace ecu::core::v2::protocol::j1939 {
namespace {

[[nodiscard]] bool is_supported_multi_pg_destination(
    const std::uint8_t address) noexcept {
  return is_claimable_address(address) ||
         address == kGlobalAddress;
}

[[nodiscard]] std::uint8_t next_fd_length(
    const std::size_t used) noexcept {
  if (used <= 8U) {
    return static_cast<std::uint8_t>(used);
  }
  if (used <= 12U) {
    return 12U;
  }
  if (used <= 16U) {
    return 16U;
  }
  if (used <= 20U) {
    return 20U;
  }
  if (used <= 24U) {
    return 24U;
  }
  if (used <= 32U) {
    return 32U;
  }
  if (used <= 48U) {
    return 48U;
  }
  if (used <= 64U) {
    return 64U;
  }
  return 0U;
}

// This is the deterministic padding profile emitted by the public J1939-22
// reference implementation used for the engineering cross-check: the padding
// service starts with up to three zero bytes and any remaining DLC fill bytes
// are 0xAA. The first zero byte carries TOS=0 (padding).
[[nodiscard]] bool supported_padding_from(
    const transport::CanFrame& frame,
    const std::size_t offset) noexcept {
  const auto length =
      static_cast<std::size_t>(frame.length);
  if (offset >= length) {
    return false;
  }

  std::size_t relative = 0U;
  for (std::size_t index = offset;
       index < length;
       ++index, ++relative) {
    const auto expected =
        relative < 3U
            ? std::byte{0U}
            : std::byte{0xAAU};
    if (frame.payload[index] != expected) {
      return false;
    }
  }
  return true;
}

void write_supported_padding(
    transport::CanFrame& frame,
    const std::size_t used,
    const std::size_t encoded_length) noexcept {
  std::size_t relative = 0U;
  for (std::size_t index = used;
       index < encoded_length;
       ++index, ++relative) {
    frame.payload[index] =
        relative < 3U
            ? std::byte{0U}
            : std::byte{0xAAU};
  }
}

}  // namespace

bool encode_no_assurance_contained_pg_header(
    const std::uint32_t pgn,
    const std::uint8_t payload_length,
    std::array<std::byte, kContainedPgHeaderBytes>& header) noexcept {
  if (!is_canonical_pgn(pgn) ||
      payload_length > kContainedPgMaxPayloadBytes) {
    return false;
  }

  header[0U] = static_cast<std::byte>(
      (kContainedPgTosSaeNoAssurance << 5U) |
      ((pgn >> 16U) & 0x03U));
  header[1U] =
      static_cast<std::byte>((pgn >> 8U) & 0xFFU);
  header[2U] =
      static_cast<std::byte>(pgn & 0xFFU);
  header[3U] =
      static_cast<std::byte>(payload_length);
  return true;
}

ContainedPgDecodeStatus decode_no_assurance_contained_pg_header(
    const std::array<std::byte, kContainedPgHeaderBytes>& header,
    ContainedPgHeader& decoded) noexcept {
  const auto byte0 =
      std::to_integer<std::uint8_t>(header[0U]);
  const auto tos =
      static_cast<std::uint8_t>(
          (byte0 >> 5U) & 0x07U);
  const auto trailer =
      static_cast<std::uint8_t>(
          (byte0 >> 2U) & 0x07U);

  if (tos == kContainedPgTosPadding) {
    return ContainedPgDecodeStatus::padding;
  }
  if (tos != kContainedPgTosSaeNoAssurance ||
      trailer != kContainedPgTrailerNone) {
    return ContainedPgDecodeStatus::unsupported_profile;
  }

  const auto pgn =
      (static_cast<std::uint32_t>(
           byte0 & 0x03U)
       << 16U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(
               header[1U]))
       << 8U) |
      static_cast<std::uint32_t>(
          std::to_integer<std::uint8_t>(
              header[2U]));
  const auto payload_length =
      std::to_integer<std::uint8_t>(
          header[3U]);

  if (!is_canonical_pgn(pgn) ||
      payload_length > kContainedPgMaxPayloadBytes) {
    return ContainedPgDecodeStatus::invalid_argument;
  }

  decoded.type_of_service = tos;
  decoded.trailer_format = trailer;
  decoded.pgn = pgn;
  decoded.payload_length = payload_length;
  return ContainedPgDecodeStatus::ok;
}

bool decode_multi_pg_envelope(
    const transport::CanFrame& frame,
    MultiPgEnvelope& envelope) noexcept {
  if (!transport::is_valid_can_frame(frame) ||
      frame.format != transport::CanFrameFormat::fd ||
      frame.type != transport::CanFrameType::data ||
      !frame.bit_rate_switch) {
    return false;
  }

  if (frame.identifier_format ==
      transport::CanIdentifierFormat::standard_11_bit) {
    // FBFF Global Multi-PG: AppPI occupies the high three identifier bits and
    // is 000 for the currently supported J1939 application profile. The low
    // eight bits are the J1939 source address.
    if (frame.identifier > 0xFFU) {
      return false;
    }

    const auto source =
        static_cast<std::uint8_t>(
            frame.identifier & 0xFFU);
    if (!is_claimable_address(source)) {
      return false;
    }

    envelope = {};
    envelope.format = MultiPgOuterFormat::fbff;
    envelope.priority = kFbffApplicationPriority;
    envelope.source_address = source;
    envelope.destination_address = kGlobalAddress;
    return true;
  }

  if (frame.identifier_format !=
      transport::CanIdentifierFormat::extended_29_bit) {
    return false;
  }

  IdentifierFields fields{};
  if (!decode_identifier(frame.identifier, fields) ||
      parameter_group_number(fields) != kFdMultiPgPgn ||
      !is_claimable_address(fields.source_address)) {
    return false;
  }

  std::uint8_t destination = kNullAddress;
  if (!destination_address(fields, destination) ||
      !is_supported_multi_pg_destination(destination)) {
    return false;
  }

  envelope = {};
  envelope.format = MultiPgOuterFormat::feff;
  envelope.priority = fields.priority;
  envelope.source_address = fields.source_address;
  envelope.destination_address = destination;
  return true;
}

bool decode_extended_multi_pg_envelope(
    const transport::CanFrame& frame,
    MultiPgEnvelope& envelope) noexcept {
  MultiPgEnvelope decoded{};
  if (!decode_multi_pg_envelope(frame, decoded) ||
      decoded.format != MultiPgOuterFormat::feff) {
    return false;
  }
  envelope = decoded;
  return true;
}

ContainedPgDecodeStatus decode_contained_pg_at(
    const transport::CanFrame& frame,
    const std::size_t offset,
    ContainedPgView& view,
    std::size_t& next_offset) noexcept {
  MultiPgEnvelope envelope{};
  if (!decode_multi_pg_envelope(frame, envelope) ||
      offset >=
          static_cast<std::size_t>(frame.length)) {
    return ContainedPgDecodeStatus::invalid_argument;
  }

  const auto remaining =
      static_cast<std::size_t>(frame.length) -
      offset;
  if (frame.payload[offset] == std::byte{0U}) {
    return supported_padding_from(frame, offset)
               ? ContainedPgDecodeStatus::padding
               : ContainedPgDecodeStatus::invalid_argument;
  }

  if (remaining < kContainedPgHeaderBytes) {
    return ContainedPgDecodeStatus::invalid_argument;
  }

  std::array<std::byte, kContainedPgHeaderBytes>
      header{};
  for (std::size_t index = 0U;
       index < header.size();
       ++index) {
    header[index] =
        frame.payload[offset + index];
  }

  ContainedPgHeader decoded{};
  const auto status =
      decode_no_assurance_contained_pg_header(
          header,
          decoded);
  if (status != ContainedPgDecodeStatus::ok) {
    return status;
  }

  const auto required =
      kContainedPgHeaderBytes +
      static_cast<std::size_t>(
          decoded.payload_length);
  if (required > remaining) {
    return ContainedPgDecodeStatus::invalid_argument;
  }

  view.pgn = decoded.pgn;
  view.payload_length = decoded.payload_length;
  view.payload_offset =
      offset + kContainedPgHeaderBytes;
  next_offset = offset + required;
  return ContainedPgDecodeStatus::ok;
}

bool ExtendedMultiPgBuilder::begin(
    const std::uint8_t priority,
    const std::uint8_t source_address,
    const std::uint8_t destination_address) noexcept {
  std::uint32_t identifier = 0U;
  if (!is_claimable_address(source_address) ||
      !is_supported_multi_pg_destination(
          destination_address) ||
      !encode_identifier(
          MessageAddress{
              priority,
              kFdMultiPgPgn,
              source_address,
              destination_address},
          identifier)) {
    return false;
  }

  frame_ = {};
  frame_.identifier = identifier;
  frame_.identifier_format =
      transport::CanIdentifierFormat::extended_29_bit;
  frame_.format = transport::CanFrameFormat::fd;
  frame_.type = transport::CanFrameType::data;
  frame_.bit_rate_switch = true;
  used_ = 0U;
  outer_format_ = MultiPgOuterFormat::feff;
  active_ = true;
  return true;
}

bool ExtendedMultiPgBuilder::begin_fbff(
    const std::uint8_t source_address) noexcept {
  if (!is_claimable_address(source_address)) {
    return false;
  }

  frame_ = {};
  frame_.identifier =
      static_cast<std::uint32_t>(source_address);
  frame_.identifier_format =
      transport::CanIdentifierFormat::standard_11_bit;
  frame_.format = transport::CanFrameFormat::fd;
  frame_.type = transport::CanFrameType::data;
  frame_.bit_rate_switch = true;
  used_ = 0U;
  outer_format_ = MultiPgOuterFormat::fbff;
  active_ = true;
  return true;
}

bool ExtendedMultiPgBuilder::append(
    const std::uint32_t pgn,
    const std::byte* const payload,
    const std::uint8_t payload_length) noexcept {
  if (!active_ ||
      (payload_length != 0U &&
       payload == nullptr)) {
    return false;
  }

  std::array<std::byte, kContainedPgHeaderBytes>
      header{};
  if (!encode_no_assurance_contained_pg_header(
          pgn,
          payload_length,
          header)) {
    return false;
  }

  const auto required =
      header.size() +
      static_cast<std::size_t>(payload_length);
  if (used_ + required >
      frame_.payload.size()) {
    return false;
  }

  for (std::size_t index = 0U;
       index < header.size();
       ++index) {
    frame_.payload[used_ + index] =
        header[index];
  }
  for (std::size_t index = 0U;
       index <
           static_cast<std::size_t>(
               payload_length);
       ++index) {
    frame_.payload[
        used_ + header.size() + index] =
        payload[index];
  }
  used_ += required;
  return true;
}

bool ExtendedMultiPgBuilder::finalize(
    transport::CanFrame& frame) noexcept {
  if (!active_ || used_ == 0U) {
    return false;
  }

  const auto encoded_length =
      next_fd_length(used_);
  if (encoded_length == 0U) {
    return false;
  }

  write_supported_padding(
      frame_,
      used_,
      static_cast<std::size_t>(
          encoded_length));
  frame_.length = encoded_length;

  if (!transport::is_valid_can_frame(frame_)) {
    return false;
  }

  frame = frame_;
  active_ = false;
  return true;
}

}  // namespace ecu::core::v2::protocol::j1939
