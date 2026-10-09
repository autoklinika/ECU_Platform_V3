#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::uds {

inline constexpr std::size_t kMaxUdsPayloadSize = 4095U;
inline constexpr std::uint8_t kNegativeResponseSid = 0x7FU;

enum class UdsStatus : std::uint8_t {
  ok,
  in_progress,
  idle,
  busy,
  invalid_argument,
  payload_too_large,
  timeout_p2,
  timeout_p2_star,
  transport_error,
  protocol_error,
  negative_response,
  clock_fault,
};

enum class UdsTransportFailure : std::uint8_t {
  none,
  timeout,
  protocol_error,
  bus_off,
  queue_overflow,
  clock_fault,
  other,
};

enum class UdsNegativeResponseCode : std::uint8_t {
  general_reject = 0x10U,
  service_not_supported = 0x11U,
  sub_function_not_supported = 0x12U,
  incorrect_message_length_or_invalid_format = 0x13U,
  response_too_long = 0x14U,
  busy_repeat_request = 0x21U,
  conditions_not_correct = 0x22U,
  request_sequence_error = 0x24U,
  failure_prevents_execution = 0x26U,
  request_out_of_range = 0x31U,
  security_access_denied = 0x33U,
  authentication_required = 0x34U,
  invalid_key = 0x35U,
  exceed_number_of_attempts = 0x36U,
  required_time_delay_not_expired = 0x37U,
  secure_data_transmission_required = 0x38U,
  secure_data_transmission_not_allowed = 0x39U,
  secure_data_verification_failed = 0x3AU,
  certificate_verification_failed_invalid_time_period = 0x50U,
  certificate_verification_failed_invalid_signature = 0x51U,
  certificate_verification_failed_invalid_chain_of_trust = 0x52U,
  certificate_verification_failed_invalid_type = 0x53U,
  certificate_verification_failed_invalid_format = 0x54U,
  certificate_verification_failed_invalid_content = 0x55U,
  certificate_verification_failed_invalid_scope = 0x56U,
  certificate_verification_failed_invalid_certificate = 0x57U,
  ownership_verification_failed = 0x58U,
  challenge_calculation_failed = 0x59U,
  setting_access_rights_failed = 0x5AU,
  session_key_creation_derivation_failed = 0x5BU,
  configuration_data_usage_failed = 0x5CU,
  deauthentication_failed = 0x5DU,
  upload_download_not_accepted = 0x70U,
  transfer_data_suspended = 0x71U,
  general_programming_failure = 0x72U,
  wrong_block_sequence_counter = 0x73U,
  request_correctly_received_response_pending = 0x78U,
  sub_function_not_supported_in_active_session = 0x7EU,
  service_not_supported_in_active_session = 0x7FU,
  rpm_too_high = 0x81U,
  rpm_too_low = 0x82U,
  engine_is_running = 0x83U,
  engine_is_not_running = 0x84U,
  engine_run_time_too_low = 0x85U,
  temperature_too_high = 0x86U,
  temperature_too_low = 0x87U,
  vehicle_speed_too_high = 0x88U,
  vehicle_speed_too_low = 0x89U,
  throttle_pedal_too_high = 0x8AU,
  throttle_pedal_too_low = 0x8BU,
  transmission_range_not_in_neutral = 0x8CU,
  transmission_range_not_in_gear = 0x8DU,
  brake_switch_not_closed = 0x8FU,
  shifter_lever_not_in_park = 0x90U,
  torque_converter_clutch_locked = 0x91U,
  voltage_too_high = 0x92U,
  voltage_too_low = 0x93U,
  resource_temporarily_not_available = 0x94U,
};

struct UdsTiming {
  time::MonotonicDuration p2{50000000LL};
  time::MonotonicDuration p2_star{5000000000LL};
};

struct UdsClientConfig {
  UdsTiming timing{};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
};

struct UdsRequest {
  std::size_t length{0U};
  std::array<std::byte, kMaxUdsPayloadSize> payload{};
};

struct UdsRequestView {
  const std::byte* payload{nullptr};
  std::size_t length{0U};
};

struct UdsResponse {
  UdsStatus status{UdsStatus::idle};
  std::uint8_t request_sid{0U};
  std::uint8_t response_sid{0U};
  std::uint8_t negative_response_code{0U};
  UdsTransportFailure transport_failure{UdsTransportFailure::none};
  std::size_t length{0U};
  time::MonotonicClockReading completion_timestamp{};
  std::array<std::byte, kMaxUdsPayloadSize> payload{};
};

[[nodiscard]] bool is_valid_uds_timing(
    const UdsTiming& timing) noexcept;

[[nodiscard]] bool is_valid_uds_client_config(
    const UdsClientConfig& config) noexcept;

[[nodiscard]] bool positive_response_sid(
    std::uint8_t request_sid,
    std::uint8_t& response_sid) noexcept;

}  // namespace ecu::core::v2::protocol::uds
