#pragma once

#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"
#include "ecu/core_v2/protocol/j1939/j1939_name.hpp"

#include <cstdint>

namespace ecu::core::v2::protocol::isobus {

inline constexpr std::uint32_t kWorkingSetMemberPgn = 0xFE0CU;
inline constexpr std::uint32_t kWorkingSetMasterPgn = 0xFE0DU;
inline constexpr std::uint8_t kWorkingSetPriority = 7U;
inline constexpr std::uint8_t kMinWorkingSetMembers = 1U;
inline constexpr std::uint8_t kMaxWorkingSetMembers = 250U;

struct WorkingSetMasterMessage {
  std::uint8_t source_address{j1939::kNullAddress};
  std::uint8_t member_count{0U};
};

struct WorkingSetMemberMessage {
  std::uint8_t source_address{j1939::kNullAddress};
  std::uint64_t member_name{0U};
};

[[nodiscard]] bool decode_working_set_master(
    const transport::CanFrame& frame,
    WorkingSetMasterMessage& message) noexcept;

[[nodiscard]] bool build_working_set_master(
    std::uint8_t source_address,
    std::uint8_t member_count,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool decode_working_set_member(
    const transport::CanFrame& frame,
    WorkingSetMemberMessage& message) noexcept;

[[nodiscard]] bool build_working_set_member(
    std::uint8_t source_address,
    std::uint64_t member_name,
    transport::CanFrame& frame) noexcept;

}  // namespace ecu::core::v2::protocol::isobus
