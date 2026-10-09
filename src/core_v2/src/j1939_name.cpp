#include "ecu/core_v2/protocol/j1939/j1939_name.hpp"

namespace ecu::core::v2::protocol::j1939 {

bool is_valid_name(
    const NameFields& fields) noexcept {
  return fields.identity_number <= 0x1FFFFFU &&
         fields.manufacturer_code <= 0x07FFU &&
         fields.ecu_instance <= 0x07U &&
         fields.function_instance <= 0x1FU &&
         fields.vehicle_system <= 0x7FU &&
         fields.vehicle_system_instance <= 0x0FU &&
         fields.industry_group <= 0x07U &&
         !fields.reserved;
}

bool encode_name(
    const NameFields& fields,
    std::uint64_t& raw_name) noexcept {
  if (!is_valid_name(fields)) {
    return false;
  }

  raw_name =
      static_cast<std::uint64_t>(fields.identity_number) |
      (static_cast<std::uint64_t>(fields.manufacturer_code) << 21U) |
      (static_cast<std::uint64_t>(fields.ecu_instance) << 32U) |
      (static_cast<std::uint64_t>(fields.function_instance) << 35U) |
      (static_cast<std::uint64_t>(fields.function) << 40U) |
      (static_cast<std::uint64_t>(fields.vehicle_system) << 49U) |
      (static_cast<std::uint64_t>(fields.vehicle_system_instance) << 56U) |
      (static_cast<std::uint64_t>(fields.industry_group) << 60U) |
      (static_cast<std::uint64_t>(
           fields.arbitrary_address_capable ? 1U : 0U) << 63U);
  return true;
}

NameFields decode_name(
    const std::uint64_t raw_name) noexcept {
  NameFields fields{};
  fields.identity_number =
      static_cast<std::uint32_t>(raw_name & 0x1FFFFFU);
  fields.manufacturer_code =
      static_cast<std::uint16_t>((raw_name >> 21U) & 0x07FFU);
  fields.ecu_instance =
      static_cast<std::uint8_t>((raw_name >> 32U) & 0x07U);
  fields.function_instance =
      static_cast<std::uint8_t>((raw_name >> 35U) & 0x1FU);
  fields.function =
      static_cast<std::uint8_t>((raw_name >> 40U) & 0xFFU);
  fields.reserved = ((raw_name >> 48U) & 0x01U) != 0U;
  fields.vehicle_system =
      static_cast<std::uint8_t>((raw_name >> 49U) & 0x7FU);
  fields.vehicle_system_instance =
      static_cast<std::uint8_t>((raw_name >> 56U) & 0x0FU);
  fields.industry_group =
      static_cast<std::uint8_t>((raw_name >> 60U) & 0x07U);
  fields.arbitrary_address_capable =
      ((raw_name >> 63U) & 0x01U) != 0U;
  return fields;
}

bool encode_name_payload(
    const NameFields& fields,
    std::array<std::byte, 8U>& payload) noexcept {
  std::uint64_t raw_name = 0U;
  if (!encode_name(fields, raw_name)) {
    return false;
  }

  for (std::size_t i = 0U; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>(
        (raw_name >> (i * 8U)) & 0xFFU);
  }
  return true;
}

std::uint64_t decode_name_payload(
    const std::array<std::byte, 8U>& payload) noexcept {
  std::uint64_t raw_name = 0U;
  for (std::size_t i = 0U; i < payload.size(); ++i) {
    raw_name |=
        static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(payload[i]))
        << (i * 8U);
  }
  return raw_name;
}

}  // namespace ecu::core::v2::protocol::j1939
