#pragma once

#include "ecu/core_v2/transport/can_types.hpp"

#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

inline constexpr std::uint8_t kNullAddress = 0xFEU;
inline constexpr std::uint8_t kGlobalAddress = 0xFFU;
inline constexpr std::uint8_t kMaxClaimableAddress = 0xFDU;
inline constexpr std::uint32_t kMaxPgn = 0x1FFFFU;
inline constexpr std::uint32_t kMaxCanIdentifier = 0x1FFFFFFFU;

inline constexpr std::uint32_t kRequestPgn = 0xEA00U;
inline constexpr std::uint32_t kAddressClaimedPgn = 0xEE00U;
inline constexpr std::uint32_t kCommandedAddressPgn = 0xFED8U;

struct IdentifierFields {
  std::uint8_t priority{0U};
  bool reserved{false};
  bool data_page{false};
  std::uint8_t pdu_format{0U};
  std::uint8_t pdu_specific{0U};
  std::uint8_t source_address{0U};
};

struct MessageAddress {
  std::uint8_t priority{6U};
  std::uint32_t pgn{0U};
  std::uint8_t source_address{0U};
  std::uint8_t destination_address{kGlobalAddress};
};

[[nodiscard]] constexpr bool is_pdu1(
    const IdentifierFields& fields) noexcept {
  return fields.pdu_format < 240U;
}

[[nodiscard]] constexpr bool is_claimable_address(
    const std::uint8_t address) noexcept {
  return address <= kMaxClaimableAddress;
}

[[nodiscard]] constexpr bool is_valid_source_address(
    const std::uint8_t address) noexcept {
  return address != kGlobalAddress;
}

[[nodiscard]] bool decode_identifier(
    std::uint32_t identifier,
    IdentifierFields& fields) noexcept;

[[nodiscard]] bool decode_classic_frame_identifier(
    const transport::CanFrame& frame,
    IdentifierFields& fields) noexcept;

[[nodiscard]] std::uint32_t parameter_group_number(
    const IdentifierFields& fields) noexcept;

[[nodiscard]] bool destination_address(
    const IdentifierFields& fields,
    std::uint8_t& destination) noexcept;

[[nodiscard]] bool encode_identifier(
    const MessageAddress& address,
    std::uint32_t& identifier) noexcept;

[[nodiscard]] bool build_classic_data_frame(
    const MessageAddress& address,
    const std::uint8_t* payload,
    std::uint8_t length,
    transport::CanFrame& frame) noexcept;

}  // namespace ecu::core::v2::protocol::j1939
