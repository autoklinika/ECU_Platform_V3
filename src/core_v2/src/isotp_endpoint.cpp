#include "ecu/core_v2/protocol/isotp/isotp_endpoint.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ecu::core::v2::protocol::isotp {
namespace {

constexpr std::uint8_t kSingleFrame = 0x00U;
constexpr std::uint8_t kFirstFrame = 0x10U;
constexpr std::uint8_t kConsecutiveFrame = 0x20U;
constexpr std::uint8_t kFlowControl = 0x30U;

constexpr std::uint8_t kFlowContinueToSend = 0x00U;
constexpr std::uint8_t kFlowWait = 0x01U;
constexpr std::uint8_t kFlowOverflow = 0x02U;

[[nodiscard]] std::uint8_t byte_value(const std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] bool valid_identifier(
    const std::uint32_t identifier,
    const transport::CanIdentifierFormat format) noexcept {
  switch (format) {
    case transport::CanIdentifierFormat::standard_11_bit:
      return identifier <= 0x7FFU;
    case transport::CanIdentifierFormat::extended_29_bit:
      return identifier <= 0x1FFFFFFFU;
  }
  return false;
}

[[nodiscard]] bool valid_fd_data_length(
    const std::uint8_t length) noexcept {
  if (length <= 8U) {
    return true;
  }

  switch (length) {
    case 12U:
    case 16U:
    case 20U:
    case 24U:
    case 32U:
    case 48U:
    case 64U:
      return true;
    default:
      return false;
  }
}

void copy_bytes(
    std::byte* const destination,
    const std::byte* const source,
    const std::size_t length) noexcept {
  for (std::size_t index = 0U; index < length; ++index) {
    destination[index] = source[index];
  }
}

}  // namespace

bool is_valid_isotp_address(const IsoTpAddress& address) noexcept {
  return valid_identifier(address.tx_id, address.identifier_format) &&
         valid_identifier(address.rx_id, address.identifier_format) &&
         address.tx_id != address.rx_id;
}

bool is_valid_isotp_config(const IsoTpConfig& config) noexcept {
  if (config.flow_control_timeout.count() <= 0 ||
      config.consecutive_frame_timeout.count() <= 0 ||
      !config.timestamp_domain.valid() ||
      config.max_timestamp_uncertainty.count() < 0) {
    return false;
  }

  bool stmin_valid = false;
  static_cast<void>(decode_stmin(config.rx_stmin, stmin_valid));
  if (!stmin_valid) {
    return false;
  }

  if (config.frame_format == transport::CanFrameFormat::classic) {
    return config.tx_data_length == 8U &&
           !config.bit_rate_switch;
  }

  return valid_fd_data_length(config.tx_data_length) &&
         config.tx_data_length >= 8U;
}

time::MonotonicDuration decode_stmin(
    const std::uint8_t encoded,
    bool& valid) noexcept {
  if (encoded <= 0x7FU) {
    valid = true;
    return time::MonotonicDuration{
        static_cast<std::int64_t>(encoded) * 1000000LL};
  }

  if (encoded >= 0xF1U && encoded <= 0xF9U) {
    valid = true;
    return time::MonotonicDuration{
        static_cast<std::int64_t>(encoded - 0xF0U) * 100000LL};
  }

  valid = false;
  return time::MonotonicDuration{0};
}

IsoTpEndpoint::IsoTpEndpoint(
    const IsoTpAddress address,
    const IsoTpConfig config) noexcept
    : address_(address),
      config_(config),
      valid_(
          is_valid_isotp_address(address) &&
          is_valid_isotp_config(config)) {}

bool IsoTpEndpoint::valid() const noexcept {
  return valid_;
}

IsoTpStatus IsoTpEndpoint::start_send(
    const std::byte* const payload,
    const std::size_t length) noexcept {
  if (!valid_ || payload == nullptr || length == 0U) {
    return IsoTpStatus::invalid_argument;
  }
  if (faulted_) {
    return IsoTpStatus::clock_fault;
  }
  if (length > kMaxPayloadSize) {
    return IsoTpStatus::payload_too_large;
  }
  if (tx_state_ != TxState::idle) {
    return IsoTpStatus::busy;
  }

  copy_bytes(tx_payload_.data(), payload, length);
  tx_length_ = length;
  tx_offset_ = 0U;
  tx_sequence_ = 1U;
  tx_block_size_ = 0U;
  tx_block_sent_ = 0U;
  tx_wait_frames_ = 0U;
  tx_stmin_ = time::MonotonicDuration{0};
  tx_next_send_ = time::MonotonicTime{0};
  tx_deadline_ = time::MonotonicTime{0};
  tx_completion_timestamp_ = {};
  last_tx_status_ = IsoTpStatus::in_progress;

  tx_state_ =
      length <= single_frame_capacity()
          ? TxState::single_pending
          : TxState::first_pending;
  return IsoTpStatus::in_progress;
}

IsoTpStatus IsoTpEndpoint::service(
    transport::CanBusRuntime& runtime,
    const time::MonotonicClockReading& now) noexcept {
  if (!valid_) {
    return IsoTpStatus::invalid_argument;
  }
  if (faulted_) {
    return IsoTpStatus::clock_fault;
  }
  if (!observe_service_time(now)) {
    return IsoTpStatus::clock_fault;
  }

  if (rx_active_ && deadline_reached(now, rx_deadline_)) {
    reset_rx_transfer();
    last_rx_status_ = IsoTpStatus::timeout;
    post_event(IsoTpStatus::timeout);
  }

  if (tx_state_ == TxState::waiting_flow_control &&
      deadline_reached(now, tx_deadline_)) {
    fail_tx(IsoTpStatus::timeout);
    return IsoTpStatus::timeout;
  }

  if (pending_event_ != IsoTpStatus::idle) {
    const auto event = pending_event_;
    pending_event_ = IsoTpStatus::idle;
    return event;
  }

  if (pending_control_) {
    const auto status = send_pending_control(runtime);
    if (status == IsoTpStatus::would_block) {
      return status;
    }
    if (status != IsoTpStatus::ok) {
      last_rx_status_ = status;
      reset_rx_transfer();
      return status;
    }
    return tx_busy() || rx_active_
               ? IsoTpStatus::in_progress
               : IsoTpStatus::ok;
  }

  const auto tx_status = service_tx(runtime, now);
  if (tx_status == IsoTpStatus::would_block) {
    return tx_status;
  }
  if (tx_status != IsoTpStatus::ok &&
      tx_status != IsoTpStatus::in_progress &&
      tx_status != IsoTpStatus::idle) {
    return tx_status;
  }

  if (tx_busy() || rx_active_ || pending_control_) {
    return IsoTpStatus::in_progress;
  }
  return tx_status == IsoTpStatus::idle
             ? IsoTpStatus::ok
             : tx_status;
}

void IsoTpEndpoint::on_can_frame(
    const transport::ReceivedCanFrame& received) noexcept {
  if (!valid_ || faulted_) {
    return;
  }

  const auto status = process_incoming(received);
  switch (status) {
    case IsoTpStatus::ok:
    case IsoTpStatus::in_progress:
    case IsoTpStatus::idle:
      break;
    default:
      post_event(status);
      break;
  }
}

bool IsoTpEndpoint::tx_busy() const noexcept {
  return tx_state_ != TxState::idle;
}

IsoTpStatus IsoTpEndpoint::last_tx_status() const noexcept {
  return last_tx_status_;
}

time::MonotonicClockReading
IsoTpEndpoint::tx_completion_timestamp() const noexcept {
  return tx_completion_timestamp_;
}

IsoTpStatus IsoTpEndpoint::last_rx_status() const noexcept {
  return last_rx_status_;
}

bool IsoTpEndpoint::has_received() const noexcept {
  return rx_complete_;
}

std::size_t IsoTpEndpoint::peek_received_length() const noexcept {
  return rx_complete_ ? rx_length_ : 0U;
}

IsoTpReceiveResult IsoTpEndpoint::take_received() noexcept {
  IsoTpReceiveResult result{};
  if (!rx_complete_) {
    return result;
  }

  result.status = IsoTpStatus::ok;
  result.length = rx_length_;
  result.completion_timestamp = rx_completion_timestamp_;
  copy_bytes(
      result.payload.data(),
      rx_payload_.data(),
      rx_length_);

  rx_complete_ = false;
  rx_length_ = 0U;
  rx_offset_ = 0U;
  rx_completion_timestamp_ = {};
  if (last_rx_status_ == IsoTpStatus::ok) {
    last_rx_status_ = IsoTpStatus::idle;
  }
  return result;
}

bool IsoTpEndpoint::pending_control() const noexcept {
  return pending_control_;
}

void IsoTpEndpoint::reset() noexcept {
  faulted_ = false;

  tx_state_ = TxState::idle;
  last_tx_status_ = IsoTpStatus::idle;
  tx_length_ = 0U;
  tx_offset_ = 0U;
  tx_sequence_ = 1U;
  tx_block_size_ = 0U;
  tx_block_sent_ = 0U;
  tx_wait_frames_ = 0U;
  tx_stmin_ = time::MonotonicDuration{0};
  tx_next_send_ = time::MonotonicTime{0};
  tx_deadline_ = time::MonotonicTime{0};
  tx_completion_timestamp_ = {};

  rx_complete_ = false;
  last_rx_status_ = IsoTpStatus::idle;
  rx_length_ = 0U;
  rx_offset_ = 0U;
  reset_rx_transfer();

  pending_control_ = false;
  control_frame_ = {};
  pending_event_ = IsoTpStatus::idle;

  last_service_time_ = time::MonotonicTime{0};
  last_receive_time_ = time::MonotonicTime{0};
  has_last_service_time_ = false;
  has_last_receive_time_ = false;
}

IsoTpStatus IsoTpEndpoint::process_incoming(
    const transport::ReceivedCanFrame& received) noexcept {
  const auto& frame = received.frame;
  if (frame.identifier != address_.rx_id ||
      frame.identifier_format != address_.identifier_format ||
      frame.type != transport::CanFrameType::data ||
      frame.format != config_.frame_format ||
      frame.length == 0U) {
    return IsoTpStatus::ok;
  }

  if (!transport::is_valid_can_frame(frame)) {
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }
  if (!observe_receive_time(received.timestamp)) {
    return IsoTpStatus::clock_fault;
  }

  const auto pci = byte_value(frame.payload[0U]);
  switch (pci & 0xF0U) {
    case kSingleFrame:
      return process_single_frame(frame, received.timestamp);
    case kFirstFrame:
      return process_first_frame(frame, received.timestamp);
    case kConsecutiveFrame:
      return process_consecutive_frame(frame, received.timestamp);
    case kFlowControl:
      return process_flow_control(frame, received.timestamp);
    default:
      last_rx_status_ = IsoTpStatus::protocol_error;
      return IsoTpStatus::protocol_error;
  }
}

IsoTpStatus IsoTpEndpoint::process_flow_control(
    const transport::CanFrame& frame,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (tx_state_ != TxState::waiting_flow_control) {
    return IsoTpStatus::ok;
  }
  if (deadline_reached(timestamp, tx_deadline_)) {
    fail_tx(IsoTpStatus::timeout);
    return IsoTpStatus::timeout;
  }
  if (frame.length < 3U) {
    fail_tx(IsoTpStatus::protocol_error);
    return IsoTpStatus::protocol_error;
  }

  const auto flow_status =
      static_cast<std::uint8_t>(byte_value(frame.payload[0U]) & 0x0FU);
  if (flow_status == kFlowContinueToSend) {
    bool valid_stmin = false;
    const auto stmin =
        decode_stmin(byte_value(frame.payload[2U]), valid_stmin);
    if (!valid_stmin) {
      fail_tx(IsoTpStatus::protocol_error);
      return IsoTpStatus::protocol_error;
    }

    tx_block_size_ = byte_value(frame.payload[1U]);
    tx_block_sent_ = 0U;
    tx_wait_frames_ = 0U;
    tx_stmin_ = stmin;
    const auto first_allowed = lower_bound(timestamp);
    if (tx_next_send_ < first_allowed) {
      tx_next_send_ = first_allowed;
    }
    tx_state_ = TxState::sending_consecutive;
    last_tx_status_ = IsoTpStatus::in_progress;
    return IsoTpStatus::in_progress;
  }

  if (flow_status == kFlowWait) {
    ++tx_wait_frames_;
    if (tx_wait_frames_ > config_.max_wait_frames) {
      fail_tx(IsoTpStatus::timeout);
      return IsoTpStatus::timeout;
    }
    if (!set_deadline_after(
            timestamp,
            config_.flow_control_timeout,
            tx_deadline_)) {
      latch_clock_fault();
      return IsoTpStatus::clock_fault;
    }
    return IsoTpStatus::in_progress;
  }

  if (flow_status == kFlowOverflow) {
    fail_tx(IsoTpStatus::flow_control_overflow);
    return IsoTpStatus::flow_control_overflow;
  }

  fail_tx(IsoTpStatus::protocol_error);
  return IsoTpStatus::protocol_error;
}

IsoTpStatus IsoTpEndpoint::process_single_frame(
    const transport::CanFrame& frame,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (rx_active_ || rx_complete_) {
    last_rx_status_ = IsoTpStatus::busy;
    return IsoTpStatus::busy;
  }

  const auto first = byte_value(frame.payload[0U]);
  std::size_t payload_offset = 1U;
  std::size_t length = first & 0x0FU;

  if (length == 0U) {
    if (frame.format != transport::CanFrameFormat::fd ||
        frame.length <= 8U ||
        frame.length < 2U) {
      last_rx_status_ = IsoTpStatus::protocol_error;
      return IsoTpStatus::protocol_error;
    }

    length = byte_value(frame.payload[1U]);
    payload_offset = 2U;
    if (length <= 7U) {
      last_rx_status_ = IsoTpStatus::protocol_error;
      return IsoTpStatus::protocol_error;
    }
  }

  if (length == 0U ||
      length > kMaxPayloadSize ||
      payload_offset + length >
          static_cast<std::size_t>(frame.length)) {
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }

  copy_bytes(
      rx_payload_.data(),
      frame.payload.data() + payload_offset,
      length);
  rx_length_ = length;
  rx_offset_ = length;
  rx_completion_timestamp_ = timestamp;
  rx_complete_ = true;
  last_rx_status_ = IsoTpStatus::ok;
  return IsoTpStatus::ok;
}

IsoTpStatus IsoTpEndpoint::process_first_frame(
    const transport::CanFrame& frame,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (frame.length < 2U) {
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }

  if (rx_active_ || rx_complete_) {
    const auto queue_status = queue_flow_control(kFlowOverflow);
    last_rx_status_ =
        queue_status == IsoTpStatus::ok
            ? IsoTpStatus::busy
            : queue_status;
    return last_rx_status_;
  }

  const auto first = byte_value(frame.payload[0U]);
  const auto second = byte_value(frame.payload[1U]);
  const std::size_t total_length =
      (static_cast<std::size_t>(first & 0x0FU) << 8U) |
      static_cast<std::size_t>(second);

  if (total_length == 0U || total_length > kMaxPayloadSize) {
    const auto queue_status = queue_flow_control(kFlowOverflow);
    last_rx_status_ =
        queue_status == IsoTpStatus::ok
            ? IsoTpStatus::payload_too_large
            : queue_status;
    return last_rx_status_;
  }

  const auto incoming_single_frame_capacity =
      frame.format == transport::CanFrameFormat::fd &&
              frame.length > 8U
          ? static_cast<std::size_t>(frame.length) - 2U
          : 7U;
  if (total_length <= incoming_single_frame_capacity) {
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }

  const auto available =
      static_cast<std::size_t>(frame.length) - 2U;
  const auto copy_length =
      available < total_length ? available : total_length;
  if (copy_length == 0U) {
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }

  copy_bytes(
      rx_payload_.data(),
      frame.payload.data() + 2U,
      copy_length);

  rx_length_ = total_length;
  rx_offset_ = copy_length;
  rx_expected_sequence_ = 1U;
  rx_block_received_ = 0U;
  rx_active_ = true;
  last_rx_status_ = IsoTpStatus::in_progress;

  if (!set_deadline_after(
          timestamp,
          config_.consecutive_frame_timeout,
          rx_deadline_)) {
    latch_clock_fault();
    return IsoTpStatus::clock_fault;
  }

  const auto queue_status =
      queue_flow_control(kFlowContinueToSend);
  if (queue_status != IsoTpStatus::ok) {
    reset_rx_transfer();
    last_rx_status_ = queue_status;
    return queue_status;
  }

  return IsoTpStatus::in_progress;
}

IsoTpStatus IsoTpEndpoint::process_consecutive_frame(
    const transport::CanFrame& frame,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (!rx_active_ || frame.length < 2U) {
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }
  if (deadline_reached(timestamp, rx_deadline_)) {
    reset_rx_transfer();
    pending_control_ = false;
    control_frame_ = {};
    last_rx_status_ = IsoTpStatus::timeout;
    return IsoTpStatus::timeout;
  }
  if (pending_control_) {
    reset_rx_transfer();
    pending_control_ = false;
    control_frame_ = {};
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }

  const auto sequence =
      static_cast<std::uint8_t>(
          byte_value(frame.payload[0U]) & 0x0FU);
  if (sequence != rx_expected_sequence_) {
    reset_rx_transfer();
    last_rx_status_ = IsoTpStatus::sequence_error;
    return IsoTpStatus::sequence_error;
  }

  const auto remaining = rx_length_ - rx_offset_;
  const auto available =
      static_cast<std::size_t>(frame.length) - 1U;
  const auto copy_length =
      remaining < available ? remaining : available;
  if (copy_length == 0U) {
    reset_rx_transfer();
    last_rx_status_ = IsoTpStatus::protocol_error;
    return IsoTpStatus::protocol_error;
  }

  copy_bytes(
      rx_payload_.data() + rx_offset_,
      frame.payload.data() + 1U,
      copy_length);
  rx_offset_ += copy_length;
  rx_expected_sequence_ =
      static_cast<std::uint8_t>(
          (rx_expected_sequence_ + 1U) & 0x0FU);
  ++rx_block_received_;

  if (rx_offset_ >= rx_length_) {
    rx_active_ = false;
    rx_completion_timestamp_ = timestamp;
    rx_complete_ = true;
    last_rx_status_ = IsoTpStatus::ok;
    return IsoTpStatus::ok;
  }

  if (!set_deadline_after(
          timestamp,
          config_.consecutive_frame_timeout,
          rx_deadline_)) {
    latch_clock_fault();
    return IsoTpStatus::clock_fault;
  }

  if (config_.rx_block_size != 0U &&
      rx_block_received_ >= config_.rx_block_size) {
    rx_block_received_ = 0U;
    const auto queue_status =
        queue_flow_control(kFlowContinueToSend);
    if (queue_status != IsoTpStatus::ok) {
      reset_rx_transfer();
      last_rx_status_ = queue_status;
      return queue_status;
    }
  }

  last_rx_status_ = IsoTpStatus::in_progress;
  return IsoTpStatus::in_progress;
}

IsoTpStatus IsoTpEndpoint::service_tx(
    transport::CanBusRuntime& runtime,
    const time::MonotonicClockReading& now) noexcept {
  if (tx_state_ == TxState::idle) {
    return IsoTpStatus::idle;
  }
  if (tx_state_ == TxState::waiting_flow_control) {
    return IsoTpStatus::in_progress;
  }

  if (tx_state_ == TxState::single_pending) {
    auto frame = make_base_tx_frame();

    if (tx_length_ <= 7U) {
      frame.payload[0U] =
          static_cast<std::byte>(tx_length_);
      copy_bytes(
          frame.payload.data() + 1U,
          tx_payload_.data(),
          tx_length_);
      frame.length = choose_wire_length(
          static_cast<std::uint8_t>(tx_length_ + 1U));
    } else {
      frame.payload[0U] = std::byte{0x00U};
      frame.payload[1U] =
          static_cast<std::byte>(tx_length_);
      copy_bytes(
          frame.payload.data() + 2U,
          tx_payload_.data(),
          tx_length_);
      frame.length = choose_wire_length(
          static_cast<std::uint8_t>(tx_length_ + 2U));
    }

    if (frame.length == 0U) {
      fail_tx(IsoTpStatus::protocol_error);
      return IsoTpStatus::protocol_error;
    }

    const auto can_status = runtime.send(frame);
    if (can_status == transport::CanStatus::would_block ||
        can_status == transport::CanStatus::busy) {
      return IsoTpStatus::would_block;
    }
    if (can_status != transport::CanStatus::ok) {
      const auto mapped = map_can_status(can_status);
      fail_tx(mapped);
      return mapped;
    }

    tx_state_ = TxState::idle;
    tx_completion_timestamp_ = now;
    last_tx_status_ = IsoTpStatus::ok;
    return IsoTpStatus::ok;
  }

  if (tx_state_ == TxState::first_pending) {
    auto frame = make_base_tx_frame();
    frame.payload[0U] =
        static_cast<std::byte>(
            kFirstFrame |
            ((tx_length_ >> 8U) & 0x0FU));
    frame.payload[1U] =
        static_cast<std::byte>(tx_length_ & 0xFFU);

    const auto capacity =
        static_cast<std::size_t>(config_.tx_data_length) - 2U;
    const auto copy_length =
        capacity < tx_length_ ? capacity : tx_length_;
    copy_bytes(
        frame.payload.data() + 2U,
        tx_payload_.data(),
        copy_length);
    frame.length = config_.tx_data_length;

    const auto can_status = runtime.send(frame);
    if (can_status == transport::CanStatus::would_block ||
        can_status == transport::CanStatus::busy) {
      return IsoTpStatus::would_block;
    }
    if (can_status != transport::CanStatus::ok) {
      const auto mapped = map_can_status(can_status);
      fail_tx(mapped);
      return mapped;
    }

    tx_offset_ = copy_length;
    tx_state_ = TxState::waiting_flow_control;
    if (!set_deadline_after(
            now,
            config_.flow_control_timeout,
            tx_deadline_)) {
      latch_clock_fault();
      return IsoTpStatus::clock_fault;
    }
    return IsoTpStatus::in_progress;
  }

  if (tx_state_ != TxState::sending_consecutive) {
    fail_tx(IsoTpStatus::protocol_error);
    return IsoTpStatus::protocol_error;
  }

  if (lower_bound(now) < tx_next_send_) {
    return IsoTpStatus::in_progress;
  }

  auto frame = make_base_tx_frame();
  frame.payload[0U] =
      static_cast<std::byte>(
          kConsecutiveFrame |
          (tx_sequence_ & 0x0FU));

  const auto remaining = tx_length_ - tx_offset_;
  const auto capacity =
      static_cast<std::size_t>(config_.tx_data_length) - 1U;
  const auto copy_length =
      remaining < capacity ? remaining : capacity;
  copy_bytes(
      frame.payload.data() + 1U,
      tx_payload_.data() + tx_offset_,
      copy_length);
  frame.length = choose_wire_length(
      static_cast<std::uint8_t>(copy_length + 1U));

  if (frame.length == 0U) {
    fail_tx(IsoTpStatus::protocol_error);
    return IsoTpStatus::protocol_error;
  }

  const auto can_status = runtime.send(frame);
  if (can_status == transport::CanStatus::would_block ||
      can_status == transport::CanStatus::busy) {
    return IsoTpStatus::would_block;
  }
  if (can_status != transport::CanStatus::ok) {
    const auto mapped = map_can_status(can_status);
    fail_tx(mapped);
    return mapped;
  }

  tx_offset_ += copy_length;
  tx_sequence_ =
      static_cast<std::uint8_t>((tx_sequence_ + 1U) & 0x0FU);
  ++tx_block_sent_;

  if (tx_offset_ >= tx_length_) {
    tx_state_ = TxState::idle;
    tx_completion_timestamp_ = now;
    last_tx_status_ = IsoTpStatus::ok;
    return IsoTpStatus::ok;
  }

  if (!set_deadline_after(now, tx_stmin_, tx_next_send_)) {
    latch_clock_fault();
    return IsoTpStatus::clock_fault;
  }

  if (tx_block_size_ != 0U &&
      tx_block_sent_ >= tx_block_size_) {
    tx_block_sent_ = 0U;
    tx_state_ = TxState::waiting_flow_control;
    if (!set_deadline_after(
            now,
            config_.flow_control_timeout,
            tx_deadline_)) {
      latch_clock_fault();
      return IsoTpStatus::clock_fault;
    }
    return IsoTpStatus::in_progress;
  }

  return IsoTpStatus::in_progress;
}

IsoTpStatus IsoTpEndpoint::send_pending_control(
    transport::CanBusRuntime& runtime) noexcept {
  if (!pending_control_) {
    return IsoTpStatus::ok;
  }

  const auto status = runtime.send(control_frame_);
  if (status == transport::CanStatus::would_block ||
      status == transport::CanStatus::busy) {
    return IsoTpStatus::would_block;
  }
  if (status != transport::CanStatus::ok) {
    return map_can_status(status);
  }

  pending_control_ = false;
  control_frame_ = {};
  return IsoTpStatus::ok;
}

IsoTpStatus IsoTpEndpoint::queue_flow_control(
    const std::uint8_t flow_status) noexcept {
  if (pending_control_) {
    return IsoTpStatus::queue_overflow;
  }

  auto frame = make_base_tx_frame();
  frame.payload[0U] =
      static_cast<std::byte>(
          kFlowControl | (flow_status & 0x0FU));
  frame.payload[1U] =
      static_cast<std::byte>(config_.rx_block_size);
  frame.payload[2U] =
      static_cast<std::byte>(config_.rx_stmin);
  frame.length = choose_wire_length(3U);
  if (frame.length == 0U) {
    return IsoTpStatus::protocol_error;
  }

  control_frame_ = frame;
  pending_control_ = true;
  return IsoTpStatus::ok;
}

transport::CanFrame IsoTpEndpoint::make_base_tx_frame() const noexcept {
  transport::CanFrame frame{};
  frame.identifier = address_.tx_id;
  frame.identifier_format = address_.identifier_format;
  frame.format = config_.frame_format;
  frame.type = transport::CanFrameType::data;
  frame.bit_rate_switch =
      config_.frame_format == transport::CanFrameFormat::fd &&
      config_.bit_rate_switch;
  return frame;
}

std::uint8_t IsoTpEndpoint::choose_wire_length(
    const std::uint8_t used_bytes) const noexcept {
  if (used_bytes > config_.tx_data_length) {
    return 0U;
  }

  if (config_.frame_format == transport::CanFrameFormat::classic) {
    return used_bytes <= 8U ? used_bytes : 0U;
  }

  if (used_bytes <= 8U) {
    return used_bytes;
  }

  constexpr std::array<std::uint8_t, 7U> allowed{
      12U, 16U, 20U, 24U, 32U, 48U, 64U};
  for (const auto candidate : allowed) {
    if (candidate >= used_bytes &&
        candidate <= config_.tx_data_length) {
      return candidate;
    }
  }
  return 0U;
}

std::size_t IsoTpEndpoint::single_frame_capacity() const noexcept {
  if (config_.frame_format == transport::CanFrameFormat::classic ||
      config_.tx_data_length <= 8U) {
    return 7U;
  }
  return static_cast<std::size_t>(config_.tx_data_length) - 2U;
}

IsoTpStatus IsoTpEndpoint::map_can_status(
    const transport::CanStatus status) const noexcept {
  switch (status) {
    case transport::CanStatus::ok:
      return IsoTpStatus::ok;
    case transport::CanStatus::would_block:
    case transport::CanStatus::busy:
      return IsoTpStatus::would_block;
    case transport::CanStatus::bus_off:
      return IsoTpStatus::bus_off;
    case transport::CanStatus::not_open:
    case transport::CanStatus::already_open:
    case transport::CanStatus::invalid_argument:
    case transport::CanStatus::invalid_state:
    case transport::CanStatus::invalid_frame:
    case transport::CanStatus::invalid_timestamp:
    case transport::CanStatus::unsupported:
    case transport::CanStatus::contract_violation:
    case transport::CanStatus::io_error:
    case transport::CanStatus::faulted:
      return IsoTpStatus::transport_error;
  }
  return IsoTpStatus::transport_error;
}

bool IsoTpEndpoint::observe_service_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading, config_.timestamp_domain) ||
      reading.uncertainty >
          config_.max_timestamp_uncertainty ||
      (has_last_service_time_ &&
       reading.value < last_service_time_)) {
    latch_clock_fault();
    return false;
  }

  last_service_time_ = reading.value;
  has_last_service_time_ = true;
  return true;
}

bool IsoTpEndpoint::observe_receive_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading, config_.timestamp_domain) ||
      reading.uncertainty >
          config_.max_timestamp_uncertainty ||
      (has_last_receive_time_ &&
       reading.value < last_receive_time_)) {
    latch_clock_fault();
    return false;
  }

  last_receive_time_ = reading.value;
  has_last_receive_time_ = true;
  return true;
}

bool IsoTpEndpoint::set_deadline_after(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration delay,
    time::MonotonicTime& deadline) const noexcept {
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (reading.uncertainty.count() < 0 ||
      reading.value.count() >
          maximum - reading.uncertainty.count()) {
    return false;
  }

  const auto upper =
      reading.value.count() + reading.uncertainty.count();
  if (delay.count() < 0 ||
      upper > maximum - delay.count()) {
    return false;
  }

  deadline =
      time::MonotonicTime{upper + delay.count()};
  return true;
}

time::MonotonicTime IsoTpEndpoint::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

bool IsoTpEndpoint::deadline_reached(
    const time::MonotonicClockReading& reading,
    const time::MonotonicTime deadline) const noexcept {
  return lower_bound(reading) > deadline;
}

void IsoTpEndpoint::fail_tx(const IsoTpStatus status) noexcept {
  tx_state_ = TxState::idle;
  last_tx_status_ = status;
  tx_length_ = 0U;
  tx_offset_ = 0U;
  tx_sequence_ = 1U;
  tx_block_size_ = 0U;
  tx_block_sent_ = 0U;
  tx_wait_frames_ = 0U;
  tx_stmin_ = time::MonotonicDuration{0};
  tx_next_send_ = time::MonotonicTime{0};
  tx_deadline_ = time::MonotonicTime{0};
  tx_completion_timestamp_ = {};
}

void IsoTpEndpoint::reset_rx_transfer() noexcept {
  rx_active_ = false;
  rx_length_ = 0U;
  rx_offset_ = 0U;
  rx_expected_sequence_ = 1U;
  rx_block_received_ = 0U;
  rx_deadline_ = time::MonotonicTime{0};
  rx_completion_timestamp_ = {};
}

void IsoTpEndpoint::latch_clock_fault() noexcept {
  faulted_ = true;
  fail_tx(IsoTpStatus::clock_fault);
  reset_rx_transfer();
  rx_complete_ = false;
  last_rx_status_ = IsoTpStatus::clock_fault;
  pending_control_ = false;
  control_frame_ = {};
  pending_event_ = IsoTpStatus::clock_fault;
}

void IsoTpEndpoint::post_event(
    const IsoTpStatus status) noexcept {
  if (pending_event_ == IsoTpStatus::idle) {
    pending_event_ = status;
  }
}

}  // namespace ecu::core::v2::protocol::isotp
