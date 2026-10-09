#include "ecu/core_v2/protocol/isobus/working_set.hpp"

#include <array>
#include <cstddef>

namespace ecu::core::v2::protocol::isobus {

namespace {

[[nodiscard]] bool is_valid_member_count(
    const std::uint8_t member_count) noexcept {
  return member_count >= kMinWorkingSetMembers &&
         member_count <= kMaxWorkingSetMembers;
}

[[nodiscard]] bool is_valid_working_set_source(
    const std::uint8_t source_address) noexcept {
  return j1939::is_claimable_address(source_address);
}

[[nodiscard]] bool is_valid_raw_name(
    const std::uint64_t raw_name) noexcept {
  return j1939::is_valid_name(j1939::decode_name(raw_name));
}

}  // namespace

bool decode_working_set_master(
    const transport::CanFrame& frame,
    WorkingSetMasterMessage& message) noexcept {
  j1939::IdentifierFields fields{};
  if (frame.length != 8U ||
      !j1939::decode_classic_frame_identifier(frame, fields) ||
      j1939::parameter_group_number(fields) !=
          kWorkingSetMasterPgn ||
      !is_valid_working_set_source(fields.source_address)) {
    return false;
  }

  const auto member_count =
      std::to_integer<std::uint8_t>(frame.payload[0U]);
  if (!is_valid_member_count(member_count)) {
    return false;
  }

  for (std::size_t index = 1U; index < 8U; ++index) {
    if (std::to_integer<std::uint8_t>(
            frame.payload[index]) != 0xFFU) {
      return false;
    }
  }

  message.source_address = fields.source_address;
  message.member_count = member_count;
  return true;
}

bool build_working_set_master(
    const std::uint8_t source_address,
    const std::uint8_t member_count,
    transport::CanFrame& frame) noexcept {
  if (!is_valid_working_set_source(source_address) ||
      !is_valid_member_count(member_count)) {
    return false;
  }

  std::array<std::uint8_t, 8U> payload{};
  payload.fill(0xFFU);
  payload[0U] = member_count;

  return j1939::build_classic_data_frame(
      j1939::MessageAddress{
          kWorkingSetPriority,
          kWorkingSetMasterPgn,
          source_address,
          j1939::kGlobalAddress},
      payload.data(),
      static_cast<std::uint8_t>(payload.size()),
      frame);
}

bool decode_working_set_member(
    const transport::CanFrame& frame,
    WorkingSetMemberMessage& message) noexcept {
  j1939::IdentifierFields fields{};
  if (frame.length != 8U ||
      !j1939::decode_classic_frame_identifier(frame, fields) ||
      j1939::parameter_group_number(fields) !=
          kWorkingSetMemberPgn ||
      !is_valid_working_set_source(fields.source_address)) {
    return false;
  }

  std::array<std::byte, 8U> payload{};
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    payload[index] = frame.payload[index];
  }

  const auto member_name =
      j1939::decode_name_payload(payload);
  if (!is_valid_raw_name(member_name)) {
    return false;
  }

  message.source_address = fields.source_address;
  message.member_name = member_name;
  return true;
}

bool build_working_set_member(
    const std::uint8_t source_address,
    const std::uint64_t member_name,
    transport::CanFrame& frame) noexcept {
  if (!is_valid_working_set_source(source_address) ||
      !is_valid_raw_name(member_name)) {
    return false;
  }

  std::array<std::byte, 8U> payload{};
  if (!j1939::encode_name_payload(
          j1939::decode_name(member_name),
          payload)) {
    return false;
  }

  std::array<std::uint8_t, 8U> bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] =
        std::to_integer<std::uint8_t>(payload[index]);
  }

  return j1939::build_classic_data_frame(
      j1939::MessageAddress{
          kWorkingSetPriority,
          kWorkingSetMemberPgn,
          source_address,
          j1939::kGlobalAddress},
      bytes.data(),
      static_cast<std::uint8_t>(bytes.size()),
      frame);
}

}  // namespace ecu::core::v2::protocol::isobus
