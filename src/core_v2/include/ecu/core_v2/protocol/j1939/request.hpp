#pragma once

#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"

#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

struct RequestMessage {
  std::uint8_t source_address{kNullAddress};
  std::uint8_t destination_address{kGlobalAddress};
  std::uint32_t requested_pgn{0U};
};

[[nodiscard]] bool is_canonical_pgn(
    std::uint32_t pgn) noexcept;

[[nodiscard]] bool decode_request(
    const transport::CanFrame& frame,
    RequestMessage& request) noexcept;

[[nodiscard]] bool build_request(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint32_t requested_pgn,
    transport::CanFrame& frame) noexcept;

}  // namespace ecu::core::v2::protocol::j1939
