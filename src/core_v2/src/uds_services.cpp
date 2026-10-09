#include "ecu/core_v2/protocol/uds/uds_services.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::uds {
namespace {

[[nodiscard]] std::uint8_t byte_value(
    const std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

}  // namespace

UdsRequest make_diagnostic_session_control(
    const std::uint8_t session_type) noexcept {
  UdsRequest request{};
  if (session_type == 0U || session_type > 0x7FU) {
    return request;
  }

  request.length = 2U;
  request.payload[0U] =
      static_cast<std::byte>(kSidDiagnosticSessionControl);
  request.payload[1U] =
      static_cast<std::byte>(session_type);
  return request;
}

UdsRequest make_read_data_by_identifier(
    const std::uint16_t data_identifier) noexcept {
  UdsRequest request{};
  request.length = 3U;
  request.payload[0U] =
      static_cast<std::byte>(kSidReadDataByIdentifier);
  request.payload[1U] =
      static_cast<std::byte>(
          (data_identifier >> 8U) & 0xFFU);
  request.payload[2U] =
      static_cast<std::byte>(
          data_identifier & 0xFFU);
  return request;
}

UdsRequest make_read_dtc_information_by_status_mask(
    const std::uint8_t status_mask) noexcept {
  UdsRequest request{};
  request.length = 3U;
  request.payload[0U] =
      static_cast<std::byte>(kSidReadDtcInformation);
  request.payload[1U] = std::byte{0x02U};
  request.payload[2U] =
      static_cast<std::byte>(status_mask);
  return request;
}

UdsRequest make_tester_present() noexcept {
  UdsRequest request{};
  request.length = 2U;
  request.payload[0U] =
      static_cast<std::byte>(kSidTesterPresent);
  request.payload[1U] = std::byte{0x00U};
  return request;
}

bool parse_session_control_timing(
    const UdsResponse& response,
    UdsTiming& timing) noexcept {
  std::uint8_t expected_sid = 0U;
  if (!positive_response_sid(
          kSidDiagnosticSessionControl,
          expected_sid) ||
      response.status != UdsStatus::ok ||
      response.request_sid !=
          kSidDiagnosticSessionControl ||
      response.response_sid != expected_sid ||
      response.length < 6U) {
    return false;
  }

  const auto p2_raw =
      static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(
               byte_value(response.payload[2U]))
           << 8U) |
          byte_value(response.payload[3U]));
  const auto p2_star_raw =
      static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(
               byte_value(response.payload[4U]))
           << 8U) |
          byte_value(response.payload[5U]));

  UdsTiming parsed{};
  parsed.p2 = std::chrono::milliseconds{p2_raw};
  parsed.p2_star =
      std::chrono::milliseconds{
          static_cast<std::uint32_t>(
              p2_star_raw) *
          10U};

  if (!is_valid_uds_timing(parsed)) {
    return false;
  }

  timing = parsed;
  return true;
}

}  // namespace ecu::core::v2::protocol::uds
