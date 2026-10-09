#pragma once

#include "ecu/core_v2/protocol/j1939/request.hpp"

#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

inline constexpr std::uint32_t kAcknowledgmentPgn = 0xE800U;

enum class AcknowledgmentControl : std::uint8_t {
  positive = 0U,
  negative = 1U,
  access_denied = 2U,
  cannot_respond = 3U,
};

struct AcknowledgmentMessage {
  AcknowledgmentControl control{
      AcknowledgmentControl::cannot_respond};
  std::uint8_t group_function_value{0xFFU};
  std::uint8_t request_source_address{kNullAddress};
  std::uint8_t response_source_address{kNullAddress};
  std::uint32_t requested_pgn{0U};
};

[[nodiscard]] bool decode_acknowledgment(
    const transport::CanFrame& frame,
    AcknowledgmentMessage& acknowledgment) noexcept;

}  // namespace ecu::core::v2::protocol::j1939
