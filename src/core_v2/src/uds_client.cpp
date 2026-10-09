#include "ecu/core_v2/protocol/uds/uds_client.hpp"

#include "ecu/core_v2/protocol/uds/uds_services.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ecu::core::v2::protocol::uds {
namespace {

[[nodiscard]] std::uint8_t byte_value(
    const std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

void copy_bytes(
    std::byte* const destination,
    const std::byte* const source,
    const std::size_t length) noexcept {
  for (std::size_t index = 0U; index < length; ++index) {
    destination[index] = source[index];
  }
}

[[nodiscard]] bool active_transport_status(
    const transport::DiagnosticTransportStatus status) noexcept {
  return status ==
             transport::DiagnosticTransportStatus::ok ||
         status ==
             transport::DiagnosticTransportStatus::idle ||
         status ==
             transport::DiagnosticTransportStatus::in_progress ||
         status ==
             transport::DiagnosticTransportStatus::would_block;
}

}  // namespace

UdsClient::UdsClient(
    transport::IDiagnosticTransport& transport,
    const UdsClientConfig config) noexcept
    : transport_(transport),
      config_(config),
      valid_(
          transport.valid() &&
          is_valid_uds_client_config(config)) {}

bool UdsClient::valid() const noexcept {
  return valid_;
}

bool UdsClient::busy() const noexcept {
  return state_ == State::sending ||
         state_ == State::waiting_p2 ||
         state_ == State::waiting_p2_star;
}

bool UdsClient::faulted() const noexcept {
  return faulted_;
}

UdsStatus UdsClient::start_request(
    const std::byte* const payload,
    const std::size_t length) noexcept {
  if (!valid_ || payload == nullptr || length == 0U) {
    return UdsStatus::invalid_argument;
  }
  if (faulted_) {
    return UdsStatus::clock_fault;
  }
  if (length > kMaxUdsPayloadSize) {
    return UdsStatus::payload_too_large;
  }
  if (busy() || response_ready_) {
    return UdsStatus::busy;
  }

  std::uint8_t expected_sid = 0U;
  const auto request_sid = byte_value(payload[0U]);
  if (!positive_response_sid(request_sid, expected_sid)) {
    return UdsStatus::invalid_argument;
  }
  static_cast<void>(expected_sid);

  const auto transport_status =
      transport_.start_send(payload, length);
  if (transport_status !=
          transport::DiagnosticTransportStatus::in_progress &&
      transport_status !=
          transport::DiagnosticTransportStatus::ok) {
    return map_transport_status(transport_status);
  }

  request_sid_ = request_sid;
  response_ = {};
  response_ready_ = false;
  deadline_ = time::MonotonicTime{0};
  state_ = State::sending;
  return UdsStatus::in_progress;
}

UdsStatus UdsClient::start_request(
    const UdsRequest& request) noexcept {
  return start_request(
      request.payload.data(),
      request.length);
}

UdsStatus UdsClient::start_request(
    const UdsRequestView request) noexcept {
  return start_request(
      request.payload,
      request.length);
}

UdsStatus UdsClient::service(
    const time::MonotonicClockReading& now) noexcept {
  if (!valid_) {
    return UdsStatus::invalid_argument;
  }
  if (faulted_) {
    return UdsStatus::clock_fault;
  }
  if (!observe_service_time(now)) {
    return UdsStatus::clock_fault;
  }
  if (response_ready_) {
    return response_.status;
  }
  if (state_ == State::idle) {
    return UdsStatus::idle;
  }

  const auto transport_status = transport_.service(now);
  if (!active_transport_status(transport_status)) {
    const auto mapped = map_transport_status(transport_status);
    complete_with_status(
        mapped,
        map_transport_failure(transport_status));
    return mapped;
  }

  if (state_ == State::sending &&
      !transport_.tx_busy()) {
    const auto tx_status = transport_.last_tx_status();
    if (tx_status !=
        transport::DiagnosticTransportStatus::ok) {
      const auto mapped = map_transport_status(tx_status);
      complete_with_status(
          mapped,
          map_transport_failure(tx_status));
      return mapped;
    }

    const auto completion =
        transport_.tx_completion_timestamp();
    if (!observe_protocol_timestamp(completion) ||
        !set_deadline_after(
            completion,
            config_.timing.p2,
            deadline_)) {
      latch_clock_fault();
      return UdsStatus::clock_fault;
    }
    state_ = State::waiting_p2;
  }

  if (transport_.has_received()) {
    if (state_ != State::waiting_p2 &&
        state_ != State::waiting_p2_star) {
      complete_with_status(
          UdsStatus::protocol_error,
          UdsTransportFailure::none);
      return UdsStatus::protocol_error;
    }

    const auto expected_length =
        transport_.received_size();
    if (expected_length == 0U) {
      complete_with_status(
          UdsStatus::protocol_error,
          UdsTransportFailure::none);
      return UdsStatus::protocol_error;
    }
    if (expected_length >
        transport_rx_buffer_.size()) {
      complete_with_status(
          UdsStatus::payload_too_large,
          UdsTransportFailure::other);
      return UdsStatus::payload_too_large;
    }

    std::size_t received_length = 0U;
    time::MonotonicClockReading completion{};
    const auto receive_status =
        transport_.take_received(
            transport_rx_buffer_.data(),
            transport_rx_buffer_.size(),
            received_length,
            completion);
    if (receive_status !=
        transport::DiagnosticTransportStatus::ok) {
      const auto mapped =
          map_transport_status(receive_status);
      complete_with_status(
          mapped,
          map_transport_failure(receive_status));
      return mapped;
    }
    if (received_length != expected_length) {
      complete_with_status(
          UdsStatus::protocol_error,
          UdsTransportFailure::none);
      return UdsStatus::protocol_error;
    }
    if (!observe_protocol_timestamp(completion)) {
      latch_clock_fault();
      return UdsStatus::clock_fault;
    }
    if (deadline_reached(completion, deadline_)) {
      const auto status =
          state_ == State::waiting_p2
              ? UdsStatus::timeout_p2
              : UdsStatus::timeout_p2_star;
      complete_with_status(
          status,
          UdsTransportFailure::none);
      return status;
    }

    return handle_transport_response(
        transport_rx_buffer_.data(),
        received_length,
        completion);
  }

  if (state_ == State::waiting_p2 &&
      deadline_reached(now, deadline_)) {
    complete_with_status(
        UdsStatus::timeout_p2,
        UdsTransportFailure::none);
    return UdsStatus::timeout_p2;
  }
  if (state_ == State::waiting_p2_star &&
      deadline_reached(now, deadline_)) {
    complete_with_status(
        UdsStatus::timeout_p2_star,
        UdsTransportFailure::none);
    return UdsStatus::timeout_p2_star;
  }

  return busy()
             ? UdsStatus::in_progress
             : UdsStatus::idle;
}

bool UdsClient::has_response() const noexcept {
  return response_ready_;
}

UdsResponse UdsClient::take_response() noexcept {
  if (!response_ready_) {
    return UdsResponse{};
  }

  const auto result = response_;
  response_ = {};
  response_ready_ = false;
  state_ = State::idle;
  request_sid_ = 0U;
  deadline_ = time::MonotonicTime{0};
  return result;
}

UdsTiming UdsClient::timing() const noexcept {
  return config_.timing;
}

void UdsClient::set_timing(
    const UdsTiming timing) noexcept {
  if (!busy() &&
      !response_ready_ &&
      is_valid_uds_timing(timing)) {
    config_.timing = timing;
  }
}

void UdsClient::reset() noexcept {
  transport_.reset();
  faulted_ = false;
  state_ = State::idle;
  request_sid_ = 0U;
  deadline_ = time::MonotonicTime{0};
  last_service_time_ = time::MonotonicTime{0};
  has_last_service_time_ = false;
  last_protocol_time_ = time::MonotonicTime{0};
  has_last_protocol_time_ = false;
  response_ = {};
  response_ready_ = false;
}

UdsStatus UdsClient::handle_transport_response(
    const std::byte* const payload,
    const std::size_t length,
    const time::MonotonicClockReading&
        completion_timestamp) noexcept {
  if (payload == nullptr || length == 0U) {
    complete_with_status(
        UdsStatus::protocol_error,
        UdsTransportFailure::none);
    return UdsStatus::protocol_error;
  }

  const auto response_sid = byte_value(payload[0U]);
  if (response_sid == kNegativeResponseSid) {
    if (length != 3U ||
        byte_value(payload[1U]) != request_sid_) {
      complete_with_status(
          UdsStatus::protocol_error,
          UdsTransportFailure::none);
      return UdsStatus::protocol_error;
    }

    const auto nrc = byte_value(payload[2U]);
    if (nrc ==
        static_cast<std::uint8_t>(
            UdsNegativeResponseCode::
                request_correctly_received_response_pending)) {
      if (!set_deadline_after(
              completion_timestamp,
              config_.timing.p2_star,
              deadline_)) {
        latch_clock_fault();
        return UdsStatus::clock_fault;
      }
      state_ = State::waiting_p2_star;
      return UdsStatus::in_progress;
    }

    response_ = {};
    response_.status = UdsStatus::negative_response;
    response_.request_sid = request_sid_;
    response_.response_sid = kNegativeResponseSid;
    response_.negative_response_code = nrc;
    response_.length = length;
    response_.completion_timestamp =
        completion_timestamp;
    copy_bytes(
        response_.payload.data(),
        payload,
        length);
    response_ready_ = true;
    state_ = State::complete;
    return UdsStatus::negative_response;
  }

  std::uint8_t expected_sid = 0U;
  if (!positive_response_sid(
          request_sid_,
          expected_sid) ||
      response_sid != expected_sid) {
    complete_with_status(
        UdsStatus::protocol_error,
        UdsTransportFailure::none);
    return UdsStatus::protocol_error;
  }

  response_ = {};
  response_.status = UdsStatus::ok;
  response_.request_sid = request_sid_;
  response_.response_sid = response_sid;
  response_.length = length;
  response_.completion_timestamp =
      completion_timestamp;
  copy_bytes(
      response_.payload.data(),
      payload,
      length);
  response_ready_ = true;
  state_ = State::complete;

  if (request_sid_ == kSidDiagnosticSessionControl) {
    UdsTiming parsed{};
    if (parse_session_control_timing(
            response_,
            parsed)) {
      config_.timing = parsed;
    }
  }

  return UdsStatus::ok;
}

UdsStatus UdsClient::map_transport_status(
    const transport::DiagnosticTransportStatus status)
    const noexcept {
  using D = transport::DiagnosticTransportStatus;
  switch (status) {
    case D::ok:
      return UdsStatus::ok;
    case D::idle:
      return UdsStatus::idle;
    case D::in_progress:
    case D::would_block:
      return UdsStatus::in_progress;
    case D::busy:
      return UdsStatus::busy;
    case D::invalid_argument:
      return UdsStatus::invalid_argument;
    case D::payload_too_large:
      return UdsStatus::payload_too_large;
    case D::clock_fault:
      return UdsStatus::clock_fault;
    case D::timeout:
    case D::sequence_error:
    case D::flow_control_overflow:
    case D::protocol_error:
    case D::transport_error:
    case D::bus_off:
    case D::queue_overflow:
      return UdsStatus::transport_error;
  }
  return UdsStatus::transport_error;
}

UdsTransportFailure UdsClient::map_transport_failure(
    const transport::DiagnosticTransportStatus status)
    const noexcept {
  using D = transport::DiagnosticTransportStatus;
  switch (status) {
    case D::timeout:
      return UdsTransportFailure::timeout;
    case D::sequence_error:
    case D::flow_control_overflow:
    case D::protocol_error:
      return UdsTransportFailure::protocol_error;
    case D::bus_off:
      return UdsTransportFailure::bus_off;
    case D::queue_overflow:
      return UdsTransportFailure::queue_overflow;
    case D::clock_fault:
      return UdsTransportFailure::clock_fault;
    case D::transport_error:
    case D::busy:
    case D::would_block:
    case D::invalid_argument:
    case D::payload_too_large:
      return UdsTransportFailure::other;
    case D::ok:
    case D::in_progress:
    case D::idle:
      return UdsTransportFailure::none;
  }
  return UdsTransportFailure::other;
}

bool UdsClient::observe_service_time(
    const time::MonotonicClockReading& reading)
    noexcept {
  if (!valid_transport_timestamp(reading) ||
      (has_last_service_time_ &&
       reading.value < last_service_time_)) {
    latch_clock_fault();
    return false;
  }

  last_service_time_ = reading.value;
  has_last_service_time_ = true;
  return true;
}

bool UdsClient::valid_transport_timestamp(
    const time::MonotonicClockReading& reading)
    const noexcept {
  return time::is_valid_clock_reading(
             reading,
             config_.timestamp_domain) &&
         reading.uncertainty <=
             config_.max_timestamp_uncertainty;
}

bool UdsClient::observe_protocol_timestamp(
    const time::MonotonicClockReading& reading) noexcept {
  if (!valid_transport_timestamp(reading) ||
      (has_last_protocol_time_ &&
       reading.value < last_protocol_time_)) {
    return false;
  }

  last_protocol_time_ = reading.value;
  has_last_protocol_time_ = true;
  return true;
}

bool UdsClient::set_deadline_after(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration delay,
    time::MonotonicTime& deadline) const noexcept {
  const auto maximum =
      (std::numeric_limits<
          time::MonotonicTime::rep>::max)();
  if (!valid_transport_timestamp(reading) ||
      delay.count() < 0 ||
      reading.value.count() >
          maximum - reading.uncertainty.count()) {
    return false;
  }

  const auto upper =
      reading.value.count() +
      reading.uncertainty.count();
  if (upper > maximum - delay.count()) {
    return false;
  }

  deadline =
      time::MonotonicTime{
          upper + delay.count()};
  return true;
}

time::MonotonicTime UdsClient::lower_bound(
    const time::MonotonicClockReading& reading)
    const noexcept {
  if (reading.value.count() <=
      reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() -
      reading.uncertainty.count()};
}

bool UdsClient::deadline_reached(
    const time::MonotonicClockReading& reading,
    const time::MonotonicTime deadline) const noexcept {
  return lower_bound(reading) > deadline;
}

void UdsClient::complete_with_status(
    const UdsStatus status,
    const UdsTransportFailure transport_failure)
    noexcept {
  response_ = {};
  response_.status = status;
  response_.request_sid = request_sid_;
  response_.transport_failure = transport_failure;
  response_ready_ = true;
  state_ = State::complete;
}

void UdsClient::latch_clock_fault() noexcept {
  faulted_ = true;
  transport_.reset();
  complete_with_status(
      UdsStatus::clock_fault,
      UdsTransportFailure::clock_fault);
}

}  // namespace ecu::core::v2::protocol::uds
