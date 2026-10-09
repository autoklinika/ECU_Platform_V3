#include "ecu/core_v2/protocol/uds/uds_types.hpp"

namespace ecu::core::v2::protocol::uds {

bool is_valid_uds_timing(
    const UdsTiming& timing) noexcept {
  return timing.p2.count() > 0 &&
         timing.p2_star.count() > 0;
}

bool is_valid_uds_client_config(
    const UdsClientConfig& config) noexcept {
  return is_valid_uds_timing(config.timing) &&
         config.timestamp_domain.valid() &&
         config.max_timestamp_uncertainty.count() >= 0;
}

bool positive_response_sid(
    const std::uint8_t request_sid,
    std::uint8_t& response_sid) noexcept {
  if (request_sid == kNegativeResponseSid ||
      request_sid > 0xBFU) {
    response_sid = 0U;
    return false;
  }
  response_sid =
      static_cast<std::uint8_t>(request_sid + 0x40U);
  return true;
}

}  // namespace ecu::core::v2::protocol::uds
