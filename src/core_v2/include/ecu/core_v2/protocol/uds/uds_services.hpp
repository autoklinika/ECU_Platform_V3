#pragma once

#include "ecu/core_v2/protocol/uds/uds_types.hpp"

#include <cstdint>

namespace ecu::core::v2::protocol::uds {

inline constexpr std::uint8_t kSidDiagnosticSessionControl = 0x10U;
inline constexpr std::uint8_t kSidReadDtcInformation = 0x19U;
inline constexpr std::uint8_t kSidReadDataByIdentifier = 0x22U;
inline constexpr std::uint8_t kSidTesterPresent = 0x3EU;

[[nodiscard]] UdsRequest make_diagnostic_session_control(
    std::uint8_t session_type) noexcept;

[[nodiscard]] UdsRequest make_read_data_by_identifier(
    std::uint16_t data_identifier) noexcept;

[[nodiscard]] UdsRequest make_read_dtc_information_by_status_mask(
    std::uint8_t status_mask) noexcept;

[[nodiscard]] UdsRequest make_tester_present() noexcept;

[[nodiscard]] bool parse_session_control_timing(
    const UdsResponse& response,
    UdsTiming& timing) noexcept;

}  // namespace ecu::core::v2::protocol::uds
