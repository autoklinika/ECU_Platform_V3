#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

struct NameFields {
  std::uint32_t identity_number{0U};
  std::uint16_t manufacturer_code{0U};
  std::uint8_t ecu_instance{0U};
  std::uint8_t function_instance{0U};
  std::uint8_t function{0U};
  bool reserved{false};
  std::uint8_t vehicle_system{0U};
  std::uint8_t vehicle_system_instance{0U};
  std::uint8_t industry_group{0U};
  bool arbitrary_address_capable{false};
};

[[nodiscard]] bool is_valid_name(
    const NameFields& fields) noexcept;

[[nodiscard]] bool encode_name(
    const NameFields& fields,
    std::uint64_t& raw_name) noexcept;

[[nodiscard]] NameFields decode_name(
    std::uint64_t raw_name) noexcept;

[[nodiscard]] bool encode_name_payload(
    const NameFields& fields,
    std::array<std::byte, 8U>& payload) noexcept;

[[nodiscard]] std::uint64_t decode_name_payload(
    const std::array<std::byte, 8U>& payload) noexcept;

[[nodiscard]] constexpr bool name_has_higher_priority(
    const std::uint64_t lhs,
    const std::uint64_t rhs) noexcept {
  return lhs < rhs;
}

}  // namespace ecu::core::v2::protocol::j1939
