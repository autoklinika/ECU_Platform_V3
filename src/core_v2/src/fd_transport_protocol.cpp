#include "ecu/core_v2/protocol/j1939/fd_transport_protocol.hpp"

#include "ecu/core_v2/protocol/j1939/request.hpp"

#include <limits>

namespace ecu::core::v2::protocol::j1939 {
namespace {

void saturating_increment(std::uint32_t& value) noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  if (value != maximum) {
    ++value;
  }
}

[[nodiscard]] std::uint32_t decode_u24(
    const transport::CanFrame& frame,
    const std::size_t offset) noexcept {
  return
      static_cast<std::uint32_t>(
          std::to_integer<std::uint8_t>(
              frame.payload[offset])) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(
               frame.payload[offset + 1U]))
       << 8U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(
               frame.payload[offset + 2U]))
       << 16U);
}

void encode_u24(
    const std::uint32_t value,
    std::array<std::byte, 64U>& payload,
    const std::size_t offset) noexcept {
  payload[offset] =
      static_cast<std::byte>(
          value & 0xFFU);
  payload[offset + 1U] =
      static_cast<std::byte>(
          (value >> 8U) & 0xFFU);
  payload[offset + 2U] =
      static_cast<std::byte>(
          (value >> 16U) & 0xFFU);
}

[[nodiscard]] bool known_control(
    const std::uint8_t value,
    FdTpControl& control) noexcept {
  switch (value) {
    case static_cast<std::uint8_t>(
        FdTpControl::rts):
      control = FdTpControl::rts;
      return true;
    case static_cast<std::uint8_t>(
        FdTpControl::cts):
      control = FdTpControl::cts;
      return true;
    case static_cast<std::uint8_t>(
        FdTpControl::end_of_message_status):
      control = FdTpControl::end_of_message_status;
      return true;
    case static_cast<std::uint8_t>(
        FdTpControl::end_of_message_ack):
      control = FdTpControl::end_of_message_ack;
      return true;
    case static_cast<std::uint8_t>(
        FdTpControl::bam):
      control = FdTpControl::bam;
      return true;
    case static_cast<std::uint8_t>(
        FdTpControl::abort):
      control = FdTpControl::abort;
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool valid_transported_pgn(
    const std::uint32_t pgn) noexcept {
  return is_canonical_pgn(pgn) &&
         pgn != kFdTpCmPgn &&
         pgn != kFdTpDtPgn;
}

[[nodiscard]] bool valid_session_number(
    const std::uint8_t session_number) noexcept {
  return session_number <= 0x0FU;
}

[[nodiscard]] bool valid_destination(
    const std::uint8_t destination) noexcept {
  return is_claimable_address(destination) ||
         destination == kGlobalAddress;
}

[[nodiscard]] std::uint8_t next_fd_length(
    const std::size_t used) noexcept {
  if (used <= 8U) {
    return static_cast<std::uint8_t>(used);
  }
  if (used <= 12U) {
    return 12U;
  }
  if (used <= 16U) {
    return 16U;
  }
  if (used <= 20U) {
    return 20U;
  }
  if (used <= 24U) {
    return 24U;
  }
  if (used <= 32U) {
    return 32U;
  }
  if (used <= 48U) {
    return 48U;
  }
  if (used <= 64U) {
    return 64U;
  }
  return 0U;
}

[[nodiscard]] bool decode_fd_envelope(
    const transport::CanFrame& frame,
    const std::uint32_t expected_pgn,
    IdentifierFields& fields,
    std::uint8_t& destination) noexcept {
  if (!transport::is_valid_can_frame(frame) ||
      frame.identifier_format !=
          transport::CanIdentifierFormat::extended_29_bit ||
      frame.format != transport::CanFrameFormat::fd ||
      frame.type != transport::CanFrameType::data ||
      !frame.bit_rate_switch ||
      !decode_identifier(frame.identifier, fields) ||
      parameter_group_number(fields) != expected_pgn ||
      !is_claimable_address(fields.source_address) ||
      !destination_address(fields, destination) ||
      !valid_destination(destination)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool build_fd_frame(
    const std::uint8_t priority,
    const std::uint32_t pgn,
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::array<std::byte, 64U>& payload,
    const std::uint8_t length,
    transport::CanFrame& frame) noexcept {
  std::uint32_t identifier = 0U;
  if (priority > 7U ||
      !is_claimable_address(source_address) ||
      !valid_destination(destination_address) ||
      !encode_identifier(
          MessageAddress{
              priority,
              pgn,
              source_address,
              destination_address},
          identifier)) {
    return false;
  }

  transport::CanFrame built{};
  built.identifier = identifier;
  built.identifier_format =
      transport::CanIdentifierFormat::extended_29_bit;
  built.format = transport::CanFrameFormat::fd;
  built.type = transport::CanFrameType::data;
  built.bit_rate_switch = true;
  built.length = length;
  built.payload = payload;

  if (!transport::is_valid_can_frame(built)) {
    return false;
  }

  frame = built;
  return true;
}

[[nodiscard]] bool build_fd_cm(
    const std::uint8_t priority,
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const FdTpControl control,
    const std::uint8_t session_number,
    const std::uint32_t message_size,
    const std::uint32_t segment_number,
    const std::uint8_t parameter7,
    const std::uint8_t parameter8,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!valid_session_number(session_number) ||
      !valid_transported_pgn(transported_pgn)) {
    return false;
  }

  std::array<std::byte, 64U> payload{};
  payload[0U] =
      static_cast<std::byte>(
          (static_cast<std::uint8_t>(control) & 0x0FU) |
          ((session_number & 0x0FU) << 4U));
  encode_u24(message_size, payload, 1U);
  encode_u24(segment_number, payload, 4U);
  payload[7U] =
      static_cast<std::byte>(parameter7);
  payload[8U] =
      static_cast<std::byte>(parameter8);
  encode_u24(transported_pgn, payload, 9U);

  return build_fd_frame(
      priority,
      kFdTpCmPgn,
      source_address,
      destination_address,
      payload,
      static_cast<std::uint8_t>(
          kFdTpCmPayloadBytes),
      frame);
}

[[nodiscard]] bool valid_message_descriptor(
    const std::uint32_t message_size,
    const std::uint32_t segment_count) noexcept {
  return message_size >
             static_cast<std::uint32_t>(
                 kFdTpSegmentPayloadBytes) &&
         message_size <= kFdTpMaxMessageBytes &&
         segment_count >= 2U &&
         segment_count ==
             fd_tp_segment_count(message_size);
}

[[nodiscard]] bool valid_bam_descriptor(
    const std::uint32_t message_size,
    const std::uint32_t segment_count) noexcept {
  return valid_message_descriptor(
             message_size,
             segment_count) &&
         message_size <= kFdTpMaxBamMessageBytes &&
         segment_count <= kFdTpMaxBamSegments;
}

[[nodiscard]] std::uint8_t valid_bytes_for_segment(
    const std::uint32_t total_size,
    const std::uint32_t segment_number) noexcept {
  if (segment_number == 0U) {
    return 0U;
  }

  const auto offset =
      (segment_number - 1U) *
      static_cast<std::uint32_t>(
          kFdTpSegmentPayloadBytes);
  if (offset >= total_size) {
    return 0U;
  }

  const auto remaining = total_size - offset;
  return static_cast<std::uint8_t>(
      remaining <
              static_cast<std::uint32_t>(
                  kFdTpSegmentPayloadBytes)
          ? remaining
          : static_cast<std::uint32_t>(
                kFdTpSegmentPayloadBytes));
}

}  // namespace

bool decode_fd_tp_cm(
    const transport::CanFrame& frame,
    FdTpCmFrame& value) noexcept {
  IdentifierFields fields{};
  std::uint8_t destination = kNullAddress;
  if (!decode_fd_envelope(
          frame,
          kFdTpCmPgn,
          fields,
          destination) ||
      frame.length != kFdTpCmPayloadBytes) {
    return false;
  }

  const auto first =
      std::to_integer<std::uint8_t>(
          frame.payload[0U]);
  FdTpControl control{};
  if (!known_control(
          static_cast<std::uint8_t>(
              first & 0x0FU),
          control)) {
    return false;
  }

  FdTpCmFrame decoded{};
  decoded.control = control;
  decoded.key.session_number =
      static_cast<std::uint8_t>(
          (first >> 4U) & 0x0FU);
  decoded.key.source_address =
      fields.source_address;
  decoded.key.destination_address =
      destination;
  decoded.priority = fields.priority;
  decoded.message_size =
      decode_u24(frame, 1U);
  decoded.segment_number =
      decode_u24(frame, 4U);
  decoded.parameter7 =
      std::to_integer<std::uint8_t>(
          frame.payload[7U]);
  decoded.parameter8 =
      std::to_integer<std::uint8_t>(
          frame.payload[8U]);
  decoded.transported_pgn =
      decode_u24(frame, 9U);

  if (!valid_transported_pgn(
          decoded.transported_pgn)) {
    return false;
  }

  value = decoded;
  return true;
}

bool decode_fd_tp_dt(
    const transport::CanFrame& frame,
    FdTpDtFrame& value) noexcept {
  IdentifierFields fields{};
  std::uint8_t destination = kNullAddress;
  if (!decode_fd_envelope(
          frame,
          kFdTpDtPgn,
          fields,
          destination) ||
      frame.length <= kFdTpDtHeaderBytes) {
    return false;
  }

  const auto first =
      std::to_integer<std::uint8_t>(
          frame.payload[0U]);
  const auto dtfi =
      static_cast<std::uint8_t>(
          first & 0x0FU);
  if (dtfi != kFdTpNoAssuranceDtfi) {
    return false;
  }

  const auto segment_number =
      decode_u24(frame, 1U);
  if (segment_number == 0U) {
    return false;
  }

  FdTpDtFrame decoded{};
  decoded.key.session_number =
      static_cast<std::uint8_t>(
          (first >> 4U) & 0x0FU);
  decoded.key.source_address =
      fields.source_address;
  decoded.key.destination_address =
      destination;
  decoded.dtfi = dtfi;
  decoded.segment_number = segment_number;
  decoded.wire_data_bytes =
      static_cast<std::uint8_t>(
          frame.length -
          kFdTpDtHeaderBytes);
  for (auto& byte : decoded.data) {
    byte = std::byte{0xFFU};
  }
  for (std::size_t index = 0U;
       index <
           static_cast<std::size_t>(
               decoded.wire_data_bytes);
       ++index) {
    decoded.data[index] =
        frame.payload[
            kFdTpDtHeaderBytes + index];
  }

  value = decoded;
  return true;
}

bool build_fd_tp_rts(
    const std::uint8_t priority,
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t session_number,
    const std::uint32_t message_size,
    const std::uint8_t max_segments_per_cts,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  const auto segments =
      fd_tp_segment_count(message_size);
  if (!is_claimable_address(destination_address) ||
      max_segments_per_cts == 0U ||
      !valid_message_descriptor(
          message_size,
          segments)) {
    return false;
  }

  return build_fd_cm(
      priority,
      source_address,
      destination_address,
      FdTpControl::rts,
      session_number,
      message_size,
      segments,
      max_segments_per_cts,
      kFdTpNoAssuranceDataType,
      transported_pgn,
      frame);
}

bool build_fd_tp_bam(
    const std::uint8_t priority,
    const std::uint8_t source_address,
    const std::uint8_t session_number,
    const std::uint32_t message_size,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  const auto segments =
      fd_tp_segment_count(message_size);
  if (!valid_bam_descriptor(
          message_size,
          segments)) {
    return false;
  }

  return build_fd_cm(
      priority,
      source_address,
      kGlobalAddress,
      FdTpControl::bam,
      session_number,
      message_size,
      segments,
      0xFFU,
      kFdTpNoAssuranceDataType,
      transported_pgn,
      frame);
}

bool build_fd_tp_cts(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t session_number,
    const std::uint8_t segments_allowed,
    const std::uint32_t next_segment,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(destination_address) ||
      next_segment == 0U ||
      next_segment > 0xFFFFFFU) {
    return false;
  }

  return build_fd_cm(
      7U,
      source_address,
      destination_address,
      FdTpControl::cts,
      session_number,
      0xFFFFFFU,
      next_segment,
      segments_allowed,
      0U,
      transported_pgn,
      frame);
}

bool build_fd_tp_end_of_message_status(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t session_number,
    const std::uint32_t message_size,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  const auto segments =
      fd_tp_segment_count(message_size);
  const bool descriptor_valid =
      destination_address == kGlobalAddress
          ? valid_bam_descriptor(
                message_size,
                segments)
          : is_claimable_address(
                destination_address) &&
                valid_message_descriptor(
                    message_size,
                    segments);
  if (!descriptor_valid) {
    return false;
  }

  return build_fd_cm(
      7U,
      source_address,
      destination_address,
      FdTpControl::end_of_message_status,
      session_number,
      message_size,
      segments,
      0U,
      kFdTpNoAssuranceDataType,
      transported_pgn,
      frame);
}

bool build_fd_tp_end_of_message_ack(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t session_number,
    const std::uint32_t message_size,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  const auto segments =
      fd_tp_segment_count(message_size);
  if (!is_claimable_address(destination_address) ||
      !valid_message_descriptor(
          message_size,
          segments)) {
    return false;
  }

  return build_fd_cm(
      7U,
      source_address,
      destination_address,
      FdTpControl::end_of_message_ack,
      session_number,
      message_size,
      segments,
      0xFFU,
      0xFFU,
      transported_pgn,
      frame);
}

bool build_fd_tp_abort(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t session_number,
    const FdTpAbortReason reason,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(destination_address)) {
    return false;
  }

  return build_fd_cm(
      7U,
      source_address,
      destination_address,
      FdTpControl::abort,
      session_number,
      0xFFFFFFU,
      0xFFFFFFU,
      0xFFU,
      static_cast<std::uint8_t>(reason),
      transported_pgn,
      frame);
}

bool build_fd_tp_dt(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t session_number,
    const std::uint32_t segment_number,
    const std::array<std::byte, kFdTpSegmentPayloadBytes>& data,
    const std::uint8_t valid_bytes,
    transport::CanFrame& frame) noexcept {
  if (!valid_session_number(session_number) ||
      !is_claimable_address(source_address) ||
      !valid_destination(destination_address) ||
      segment_number == 0U ||
      segment_number > 0xFFFFFFU ||
      valid_bytes == 0U ||
      valid_bytes > kFdTpSegmentPayloadBytes ||
      (destination_address == kGlobalAddress &&
       segment_number > kFdTpMaxBamSegments)) {
    return false;
  }

  const auto used =
      kFdTpDtHeaderBytes +
      static_cast<std::size_t>(valid_bytes);
  const auto length =
      next_fd_length(used);
  if (length == 0U) {
    return false;
  }

  std::array<std::byte, 64U> payload{};
  payload[0U] =
      static_cast<std::byte>(
          (session_number & 0x0FU) << 4U);
  encode_u24(segment_number, payload, 1U);
  for (std::size_t index = 0U;
       index <
           static_cast<std::size_t>(
               valid_bytes);
       ++index) {
    payload[kFdTpDtHeaderBytes + index] =
        data[index];
  }
  for (std::size_t index = used;
       index < static_cast<std::size_t>(length);
       ++index) {
    payload[index] = std::byte{0xFFU};
  }

  return build_fd_frame(
      7U,
      kFdTpDtPgn,
      source_address,
      destination_address,
      payload,
      length,
      frame);
}

bool FdTpReceiver::configure(
    const FdTpReceiverConfig& config,
    IFdTpReceiveSink& sink) noexcept {
  if (!valid_config(config)) {
    return false;
  }

  config_ = config;
  sink_ = &sink;
  bam_sessions_ = {};
  peer_sessions_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  last_observed_time_ =
      time::MonotonicTime{0};
  has_last_observed_time_ = false;
  configured_ = true;
  status_ = FdTpReceiverStatus::ok;
  counters_ = {};
  return true;
}

bool FdTpReceiver::set_local_address(
    const std::uint8_t address) noexcept {
  if (!configured_ ||
      active_session_count() != 0U ||
      (address != kNullAddress &&
       !is_claimable_address(address))) {
    return false;
  }

  config_.local_address = address;
  return true;
}

void FdTpReceiver::on_can_frame(
    const transport::ReceivedCanFrame& received) noexcept {
  if (!configured_ ||
      status_ == FdTpReceiverStatus::protocol_fault ||
      status_ == FdTpReceiverStatus::queue_overflow) {
    return;
  }

  if (received.frame.identifier_format !=
          transport::CanIdentifierFormat::extended_29_bit ||
      received.frame.format !=
          transport::CanFrameFormat::fd) {
    return;
  }

  IdentifierFields fields{};
  if (!decode_identifier(
          received.frame.identifier,
          fields)) {
    return;
  }

  const auto pgn =
      parameter_group_number(fields);
  if (pgn != kFdTpCmPgn &&
      pgn != kFdTpDtPgn) {
    return;
  }

  if (!observe_time(received.timestamp)) {
    return;
  }

  if (pgn == kFdTpCmPgn) {
    FdTpCmFrame cm{};
    if (!decode_fd_tp_cm(
            received.frame,
            cm)) {
      saturating_increment(
          counters_.malformed_frames);
      return;
    }
    saturating_increment(
        counters_.cm_frames);
    handle_cm(cm, received.timestamp);
    return;
  }

  FdTpDtFrame dt{};
  if (!decode_fd_tp_dt(
          received.frame,
          dt)) {
    saturating_increment(
        counters_.malformed_frames);
    return;
  }
  saturating_increment(
      counters_.dt_frames);
  handle_dt(dt, received.timestamp);
}

FdTpReceiverStatus FdTpReceiver::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return FdTpReceiverStatus::invalid_state;
  }
  if (status_ == FdTpReceiverStatus::protocol_fault ||
      status_ == FdTpReceiverStatus::queue_overflow) {
    return status_;
  }
  if (!observe_time(now)) {
    return FdTpReceiverStatus::invalid_time;
  }

  const auto lower = lower_bound(now);
  bool acted = false;

  for (auto& session : bam_sessions_) {
    if (session.active &&
        lower > session.deadline) {
      release_session(session, true);
      saturating_increment(
          counters_.timeouts);
      acted = true;
    }
  }

  for (auto& session : peer_sessions_) {
    if (session.active &&
        lower > session.deadline) {
      const auto key = session.key;
      const auto pgn = session.pgn;
      release_session(session, true);
      static_cast<void>(
          queue_abort(
              key,
              FdTpAbortReason::timeout,
              pgn));
      saturating_increment(
          counters_.timeouts);
      acted = true;
    }
  }

  return acted
             ? status_
             : FdTpReceiverStatus::no_action;
}

bool FdTpReceiver::try_take_tx(
    transport::CanFrame& frame) noexcept {
  if (tx_count_ == 0U) {
    return false;
  }

  frame = tx_queue_[tx_head_];
  tx_head_ =
      (tx_head_ + 1U) %
      tx_queue_.size();
  --tx_count_;
  return true;
}

FdTpReceiverStatus FdTpReceiver::status() const noexcept {
  return status_;
}

FdTpReceiverCounters FdTpReceiver::counters() const noexcept {
  return counters_;
}

std::size_t FdTpReceiver::active_session_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& session : bam_sessions_) {
    if (session.active) {
      ++count;
    }
  }
  for (const auto& session : peer_sessions_) {
    if (session.active) {
      ++count;
    }
  }
  return count;
}

std::size_t FdTpReceiver::pending_tx_count() const noexcept {
  return tx_count_;
}

bool FdTpReceiver::valid_config(
    const FdTpReceiverConfig& config) const noexcept {
  return
      (config.local_address == kNullAddress ||
       is_claimable_address(
           config.local_address)) &&
      config.max_segments_per_cts != 0U &&
      config.timestamp_domain.valid() &&
      config.max_timestamp_uncertainty.count() >= 0;
}

bool FdTpReceiver::observe_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading,
          config_.timestamp_domain) ||
      reading.uncertainty >
          config_.max_timestamp_uncertainty ||
      (has_last_observed_time_ &&
       reading.value < last_observed_time_)) {
    latch_fault();
    return false;
  }

  last_observed_time_ = reading.value;
  has_last_observed_time_ = true;
  return true;
}

bool FdTpReceiver::set_deadline_after(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration delay,
    time::MonotonicTime& deadline) noexcept {
  const auto maximum =
      (std::numeric_limits<
          time::MonotonicTime::rep>::max)();
  if (reading.value.count() >
          maximum -
              reading.uncertainty.count() ||
      delay.count() < 0) {
    return false;
  }

  const auto upper =
      reading.value.count() +
      reading.uncertainty.count();
  if (upper >
      maximum - delay.count()) {
    return false;
  }

  deadline =
      time::MonotonicTime{
          upper + delay.count()};
  return true;
}

time::MonotonicTime FdTpReceiver::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <=
      reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() -
      reading.uncertainty.count()};
}

FdTpReceiver::Session* FdTpReceiver::find_session(
    const FdTpSessionKey& key) noexcept {
  for (auto& session : bam_sessions_) {
    if (session.active &&
        session.key == key) {
      return &session;
    }
  }
  for (auto& session : peer_sessions_) {
    if (session.active &&
        session.key == key) {
      return &session;
    }
  }
  return nullptr;
}

FdTpReceiver::Session* FdTpReceiver::allocate_session(
    const bool broadcast) noexcept {
  if (broadcast) {
    for (auto& session : bam_sessions_) {
      if (!session.active) {
        return &session;
      }
    }
    return nullptr;
  }

  for (auto& session : peer_sessions_) {
    if (!session.active) {
      return &session;
    }
  }
  return nullptr;
}

void FdTpReceiver::handle_cm(
    const FdTpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  switch (cm.control) {
    case FdTpControl::bam:
      start_bam(cm, timestamp);
      break;
    case FdTpControl::rts:
      start_peer(cm, timestamp);
      break;
    case FdTpControl::end_of_message_status:
      accept_eom_status(cm, timestamp);
      break;
    case FdTpControl::abort:
      accept_abort(cm);
      break;
    case FdTpControl::cts:
    case FdTpControl::end_of_message_ack:
      break;
  }
}

void FdTpReceiver::handle_dt(
    const FdTpDtFrame& dt,
    const time::MonotonicClockReading& timestamp) noexcept {
  auto* const session =
      find_session(dt.key);
  if (session == nullptr) {
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  accept_dt(
      *session,
      dt,
      timestamp);
}

void FdTpReceiver::start_bam(
    const FdTpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (cm.key.destination_address !=
          kGlobalAddress ||
      cm.parameter7 != 0xFFU ||
      cm.parameter8 !=
          kFdTpNoAssuranceDataType ||
      !valid_bam_descriptor(
          cm.message_size,
          cm.segment_number)) {
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  if (find_session(cm.key) != nullptr) {
    saturating_increment(
        counters_.rejected_sessions);
    return;
  }

  auto* const session =
      allocate_session(true);
  if (session == nullptr ||
      sink_ == nullptr) {
    saturating_increment(
        counters_.rejected_sessions);
    return;
  }

  *session = {};
  session->active = true;
  session->broadcast = true;
  session->key = cm.key;
  session->pgn = cm.transported_pgn;
  session->total_size = cm.message_size;
  session->total_segments =
      cm.segment_number;
  session->next_segment = 1U;

  if (!sink_->begin(
          session->key,
          session->pgn,
          session->total_size,
          true)) {
    *session = {};
    saturating_increment(
        counters_.sink_failures);
    return;
  }

  if (!set_deadline_after(
          timestamp,
          kSegmentTimeout,
          session->deadline)) {
    release_session(*session, true);
    latch_fault();
    return;
  }

  saturating_increment(
      counters_.bam_sessions_started);
}

void FdTpReceiver::start_peer(
    const FdTpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (config_.local_address == kNullAddress ||
      cm.key.destination_address !=
          config_.local_address) {
    return;
  }

  if (cm.parameter7 == 0U ||
      cm.parameter8 !=
          kFdTpNoAssuranceDataType ||
      !valid_message_descriptor(
          cm.message_size,
          cm.segment_number)) {
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  if (find_session(cm.key) != nullptr) {
    static_cast<void>(
        queue_abort(
            cm.key,
            FdTpAbortReason::busy,
            cm.transported_pgn));
    saturating_increment(
        counters_.rejected_sessions);
    return;
  }

  auto* const session =
      allocate_session(false);
  if (session == nullptr ||
      sink_ == nullptr) {
    static_cast<void>(
        queue_abort(
            cm.key,
            FdTpAbortReason::resources,
            cm.transported_pgn));
    saturating_increment(
        counters_.rejected_sessions);
    return;
  }

  *session = {};
  session->active = true;
  session->broadcast = false;
  session->key = cm.key;
  session->pgn = cm.transported_pgn;
  session->total_size = cm.message_size;
  session->total_segments =
      cm.segment_number;
  session->next_segment = 1U;

  if (!sink_->begin(
          session->key,
          session->pgn,
          session->total_size,
          false)) {
    const auto key = session->key;
    const auto pgn = session->pgn;
    *session = {};
    static_cast<void>(
        queue_abort(
            key,
            FdTpAbortReason::resources,
            pgn));
    saturating_increment(
        counters_.sink_failures);
    return;
  }

  const auto advertised =
      cm.parameter7 <
              config_.max_segments_per_cts
          ? cm.parameter7
          : config_.max_segments_per_cts;
  const auto remaining =
      session->total_segments;
  const auto grant =
      remaining <
              static_cast<std::uint32_t>(
                  advertised)
          ? static_cast<std::uint8_t>(
                remaining)
          : advertised;
  session->granted_remaining = grant;

  if (!queue_cts(
          *session,
          grant,
          1U) ||
      !set_deadline_after(
          timestamp,
          kCtsDataTimeout,
          session->deadline)) {
    if (session->active) {
      release_session(*session, true);
    }
    if (status_ == FdTpReceiverStatus::ok) {
      latch_fault();
    }
    return;
  }

  saturating_increment(
      counters_.peer_sessions_started);
}

void FdTpReceiver::accept_eom_status(
    const FdTpCmFrame& cm,
    const time::MonotonicClockReading&) noexcept {
  auto* const session =
      find_session(cm.key);
  if (session == nullptr) {
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  const bool valid =
      session->waiting_eom_status &&
      cm.message_size ==
          session->total_size &&
      cm.segment_number ==
          session->total_segments &&
      cm.transported_pgn ==
          session->pgn &&
      cm.parameter7 == 0U &&
      cm.parameter8 ==
          kFdTpNoAssuranceDataType;

  if (!valid) {
    const bool broadcast =
        session->broadcast;
    const auto key = session->key;
    const auto pgn = session->pgn;
    release_session(*session, true);
    if (!broadcast) {
      static_cast<void>(
          queue_abort(
              key,
              FdTpAbortReason::resources,
              pgn));
    }
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  complete_session(*session);
}

void FdTpReceiver::accept_abort(
    const FdTpCmFrame& cm) noexcept {
  auto* const session =
      find_session(cm.key);
  if (session == nullptr ||
      session->broadcast) {
    return;
  }

  release_session(*session, true);
  saturating_increment(
      counters_.remote_aborts);
}

void FdTpReceiver::accept_dt(
    Session& session,
    const FdTpDtFrame& dt,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (session.waiting_eom_status ||
      dt.segment_number !=
          session.next_segment) {
    const bool broadcast =
        session.broadcast;
    const auto key = session.key;
    const auto pgn = session.pgn;
    release_session(session, true);
    if (!broadcast) {
      static_cast<void>(
          queue_abort(
              key,
              FdTpAbortReason::resources,
              pgn));
    }
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  if (!session.broadcast &&
      session.granted_remaining == 0U) {
    const auto key = session.key;
    const auto pgn = session.pgn;
    release_session(session, true);
    static_cast<void>(
        queue_abort(
            key,
            FdTpAbortReason::resources,
            pgn));
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  const auto valid_bytes =
      valid_bytes_for_segment(
          session.total_size,
          dt.segment_number);
  if (valid_bytes == 0U ||
      dt.wire_data_bytes < valid_bytes ||
      (valid_bytes ==
           kFdTpSegmentPayloadBytes &&
       dt.wire_data_bytes !=
           kFdTpSegmentPayloadBytes)) {
    const bool broadcast =
        session.broadcast;
    const auto key = session.key;
    const auto pgn = session.pgn;
    release_session(session, true);
    if (!broadcast) {
      static_cast<void>(
          queue_abort(
              key,
              FdTpAbortReason::resources,
              pgn));
    }
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  for (std::size_t index =
           static_cast<std::size_t>(
               valid_bytes);
       index <
           static_cast<std::size_t>(
               dt.wire_data_bytes);
       ++index) {
    if (dt.data[index] !=
        std::byte{0xFFU}) {
      const bool broadcast =
          session.broadcast;
      const auto key = session.key;
      const auto pgn = session.pgn;
      release_session(session, true);
      if (!broadcast) {
        static_cast<void>(
            queue_abort(
                key,
                FdTpAbortReason::resources,
                pgn));
      }
      saturating_increment(
          counters_.malformed_frames);
      return;
    }
  }

  if (sink_ == nullptr ||
      !sink_->write(
          session.key,
          session.bytes_received,
          dt.data,
          valid_bytes)) {
    const bool broadcast =
        session.broadcast;
    const auto key = session.key;
    const auto pgn = session.pgn;
    release_session(session, true);
    if (!broadcast) {
      static_cast<void>(
          queue_abort(
              key,
              FdTpAbortReason::resources,
              pgn));
    }
    saturating_increment(
        counters_.sink_failures);
    return;
  }

  session.bytes_received +=
      static_cast<std::uint32_t>(
          valid_bytes);
  ++session.next_segment;
  if (!session.broadcast) {
    --session.granted_remaining;
  }

  if (session.bytes_received ==
          session.total_size &&
      dt.segment_number ==
          session.total_segments) {
    session.waiting_eom_status = true;
    if (!set_deadline_after(
            timestamp,
            kSegmentTimeout,
            session.deadline)) {
      release_session(session, true);
      latch_fault();
    }
    return;
  }

  if (session.bytes_received >=
          session.total_size ||
      dt.segment_number >=
          session.total_segments) {
    const bool broadcast =
        session.broadcast;
    const auto key = session.key;
    const auto pgn = session.pgn;
    release_session(session, true);
    if (!broadcast) {
      static_cast<void>(
          queue_abort(
              key,
              FdTpAbortReason::resources,
              pgn));
    }
    saturating_increment(
        counters_.malformed_frames);
    return;
  }

  if (!session.broadcast &&
      session.granted_remaining == 0U) {
    const auto segments_left =
        session.total_segments -
        (session.next_segment - 1U);
    const auto grant =
        segments_left <
                static_cast<std::uint32_t>(
                    config_.max_segments_per_cts)
            ? static_cast<std::uint8_t>(
                  segments_left)
            : config_.max_segments_per_cts;
    session.granted_remaining = grant;
    if (!queue_cts(
            session,
            grant,
            session.next_segment) ||
        !set_deadline_after(
            timestamp,
            kCtsDataTimeout,
            session.deadline)) {
      if (session.active) {
        release_session(session, true);
      }
      if (status_ == FdTpReceiverStatus::ok) {
        latch_fault();
      }
    }
    return;
  }

  if (!set_deadline_after(
          timestamp,
          kSegmentTimeout,
          session.deadline)) {
    release_session(session, true);
    latch_fault();
  }
}

bool FdTpReceiver::queue_cts(
    const Session& session,
    const std::uint8_t segments_allowed,
    const std::uint32_t next_segment) noexcept {
  transport::CanFrame frame{};
  if (!build_fd_tp_cts(
          config_.local_address,
          session.key.source_address,
          session.key.session_number,
          segments_allowed,
          next_segment,
          session.pgn,
          frame)) {
    latch_fault();
    return false;
  }
  return enqueue_tx(frame);
}

bool FdTpReceiver::queue_eom_ack(
    const Session& session) noexcept {
  transport::CanFrame frame{};
  if (!build_fd_tp_end_of_message_ack(
          config_.local_address,
          session.key.source_address,
          session.key.session_number,
          session.total_size,
          session.pgn,
          frame)) {
    latch_fault();
    return false;
  }
  return enqueue_tx(frame);
}

bool FdTpReceiver::queue_abort(
    const FdTpSessionKey& key,
    const FdTpAbortReason reason,
    const std::uint32_t pgn) noexcept {
  if (config_.local_address == kNullAddress ||
      key.destination_address !=
          config_.local_address) {
    return false;
  }

  transport::CanFrame frame{};
  if (!build_fd_tp_abort(
          config_.local_address,
          key.source_address,
          key.session_number,
          reason,
          pgn,
          frame)) {
    return false;
  }
  return enqueue_tx(frame);
}

bool FdTpReceiver::enqueue_tx(
    const transport::CanFrame& frame) noexcept {
  if (tx_count_ >= tx_queue_.size()) {
    status_ =
        FdTpReceiverStatus::queue_overflow;
    saturating_increment(
        counters_.tx_queue_overflows);
    return false;
  }

  tx_queue_[tx_tail_] = frame;
  tx_tail_ =
      (tx_tail_ + 1U) %
      tx_queue_.size();
  ++tx_count_;
  status_ = FdTpReceiverStatus::ok;
  return true;
}

void FdTpReceiver::release_session(
    Session& session,
    const bool notify_sink) noexcept {
  if (session.active &&
      notify_sink &&
      sink_ != nullptr) {
    sink_->abort(session.key);
  }
  session = {};
}

void FdTpReceiver::complete_session(
    Session& session) noexcept {
  if (sink_ == nullptr ||
      !sink_->commit(session.key)) {
    const bool broadcast =
        session.broadcast;
    const auto key = session.key;
    const auto pgn = session.pgn;
    release_session(session, true);
    if (!broadcast) {
      static_cast<void>(
          queue_abort(
              key,
              FdTpAbortReason::resources,
              pgn));
    }
    saturating_increment(
        counters_.sink_failures);
    return;
  }

  if (!session.broadcast &&
      !queue_eom_ack(session)) {
    release_session(session, false);
    return;
  }

  saturating_increment(
      counters_.completed_messages);
  release_session(session, false);
}

void FdTpReceiver::latch_fault() noexcept {
  for (auto& session : bam_sessions_) {
    release_session(session, true);
  }
  for (auto& session : peer_sessions_) {
    release_session(session, true);
  }
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  status_ = FdTpReceiverStatus::protocol_fault;
}

bool FdTpTransmitter::configure(
    const FdTpTransmitterConfig& config) noexcept {
  if (!valid_config(config)) {
    return false;
  }

  config_ = config;
  bam_sessions_ = {};
  peer_sessions_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  service_cursor_ = 0U;
  last_observed_time_ =
      time::MonotonicTime{0};
  has_last_observed_time_ = false;
  configured_ = true;
  status_ = FdTpTransmitterStatus::ok;
  counters_ = {};
  return true;
}

bool FdTpTransmitter::set_local_address(
    const std::uint8_t address) noexcept {
  if (!configured_ ||
      active_session_count() != 0U ||
      (address != kNullAddress &&
       !is_claimable_address(address))) {
    return false;
  }
  config_.local_address = address;
  return true;
}

FdTpTransmitterStatus FdTpTransmitter::submit(
    const FdTpTransmitRequest& request,
    IFdTpTransmitSource& source,
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_ ||
      status_ == FdTpTransmitterStatus::protocol_fault ||
      status_ == FdTpTransmitterStatus::queue_overflow ||
      config_.local_address == kNullAddress) {
    return FdTpTransmitterStatus::invalid_state;
  }
  if (!valid_request(request)) {
    return FdTpTransmitterStatus::invalid_argument;
  }
  if (!observe_time(now)) {
    return FdTpTransmitterStatus::invalid_time;
  }

  const bool broadcast =
      request.destination_address ==
      kGlobalAddress;
  auto* const session =
      allocate_session(broadcast);
  if (session == nullptr) {
    return FdTpTransmitterStatus::busy;
  }

  session->broadcast = broadcast;
  session->request = request;
  session->source = &source;
  session->total_segments =
      fd_tp_segment_count(
          request.total_size);
  session->next_segment = 1U;

  transport::CanFrame frame{};
  bool built = false;
  if (broadcast) {
    built = build_fd_tp_bam(
        request.priority,
        config_.local_address,
        session->session_number,
        request.total_size,
        request.pgn,
        frame);
  } else {
    built = build_fd_tp_rts(
        request.priority,
        config_.local_address,
        request.destination_address,
        session->session_number,
        request.total_size,
        config_.max_segments_per_cts,
        request.pgn,
        frame);
  }

  if (!built || !enqueue_tx(frame)) {
    reset_session(*session);
    return status_ ==
                   FdTpTransmitterStatus::queue_overflow
               ? status_
               : FdTpTransmitterStatus::protocol_fault;
  }

  if (broadcast) {
    session->phase = Phase::send_data;
    if (!set_deadline_after(
            now,
            kBamSegmentInterval,
            session->next_action)) {
      reset_session(*session);
      latch_fault();
      return FdTpTransmitterStatus::invalid_time;
    }
    saturating_increment(
        counters_.bam_sessions_started);
  } else {
    session->phase = Phase::wait_cts;
    if (!set_deadline_after(
            now,
            kResponseTimeout,
            session->deadline)) {
      reset_session(*session);
      latch_fault();
      return FdTpTransmitterStatus::invalid_time;
    }
    saturating_increment(
        counters_.peer_sessions_started);
  }

  return FdTpTransmitterStatus::ok;
}

void FdTpTransmitter::on_can_frame(
    const transport::ReceivedCanFrame& received) noexcept {
  if (!configured_ ||
      status_ == FdTpTransmitterStatus::protocol_fault ||
      status_ == FdTpTransmitterStatus::queue_overflow ||
      received.frame.identifier_format !=
          transport::CanIdentifierFormat::extended_29_bit ||
      received.frame.format !=
          transport::CanFrameFormat::fd) {
    return;
  }

  IdentifierFields fields{};
  if (!decode_identifier(
          received.frame.identifier,
          fields) ||
      parameter_group_number(fields) !=
          kFdTpCmPgn) {
    return;
  }

  if (!observe_time(received.timestamp)) {
    return;
  }

  FdTpCmFrame cm{};
  if (!decode_fd_tp_cm(
          received.frame,
          cm)) {
    saturating_increment(
        counters_.malformed_control_frames);
    return;
  }

  handle_control(
      cm,
      received.timestamp);
}

FdTpTransmitterStatus FdTpTransmitter::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return FdTpTransmitterStatus::invalid_state;
  }
  if (status_ == FdTpTransmitterStatus::protocol_fault ||
      status_ == FdTpTransmitterStatus::queue_overflow) {
    return status_;
  }
  if (!observe_time(now)) {
    return FdTpTransmitterStatus::invalid_time;
  }

  const auto lower = lower_bound(now);
  bool acted = false;

  for (auto& session : peer_sessions_) {
    if (!session.active) {
      continue;
    }
    if ((session.phase == Phase::wait_cts ||
         session.phase == Phase::wait_eom_ack) &&
        lower > session.deadline) {
      static_cast<void>(
          queue_abort(
              session,
              FdTpAbortReason::timeout));
      reset_session(session);
      saturating_increment(
          counters_.timeouts);
      acted = true;
    }
  }

  constexpr std::size_t total_slots =
      kBamSessionCapacity +
      kPeerSessionCapacity;
  for (std::size_t probe = 0U;
       probe < total_slots;
       ++probe) {
    const auto index =
        (service_cursor_ + probe) %
        total_slots;
    Session* session = nullptr;
    if (index < kBamSessionCapacity) {
      session = &bam_sessions_[index];
    } else {
      session =
          &peer_sessions_[
              index - kBamSessionCapacity];
    }

    if (!session->active) {
      continue;
    }

    if (session->phase == Phase::send_data) {
      if (lower < session->next_action) {
        continue;
      }
      if (!queue_next_dt(*session, now)) {
        if (status_ ==
            FdTpTransmitterStatus::ok) {
          reset_session(*session);
        }
      }
      service_cursor_ =
          (index + 1U) %
          total_slots;
      return status_;
    }

    if (session->phase ==
        Phase::send_eom_status) {
      if (lower < session->next_action) {
        continue;
      }
      static_cast<void>(
          queue_eom_status(*session, now));
      service_cursor_ =
          (index + 1U) %
          total_slots;
      return status_;
    }
  }

  return acted
             ? status_
             : FdTpTransmitterStatus::no_action;
}

bool FdTpTransmitter::try_take_tx(
    transport::CanFrame& frame) noexcept {
  if (tx_count_ == 0U) {
    return false;
  }

  frame = tx_queue_[tx_head_];
  tx_head_ =
      (tx_head_ + 1U) %
      tx_queue_.size();
  --tx_count_;
  return true;
}

FdTpTransmitterStatus FdTpTransmitter::status() const noexcept {
  return status_;
}

FdTpTransmitterCounters FdTpTransmitter::counters() const noexcept {
  return counters_;
}

std::size_t FdTpTransmitter::active_session_count() const noexcept {
  std::size_t count = 0U;
  for (const auto& session : bam_sessions_) {
    if (session.active) {
      ++count;
    }
  }
  for (const auto& session : peer_sessions_) {
    if (session.active) {
      ++count;
    }
  }
  return count;
}

std::size_t FdTpTransmitter::pending_tx_count() const noexcept {
  return tx_count_;
}

bool FdTpTransmitter::valid_config(
    const FdTpTransmitterConfig& config) const noexcept {
  return
      (config.local_address == kNullAddress ||
       is_claimable_address(
           config.local_address)) &&
      config.max_segments_per_cts != 0U &&
      config.timestamp_domain.valid() &&
      config.max_timestamp_uncertainty.count() >= 0;
}

bool FdTpTransmitter::valid_request(
    const FdTpTransmitRequest& request) const noexcept {
  if (!valid_transported_pgn(request.pgn) ||
      request.priority > 7U ||
      request.total_size <=
          kFdTpSegmentPayloadBytes ||
      request.total_size >
          kFdTpMaxMessageBytes ||
      !valid_destination(
          request.destination_address)) {
    return false;
  }

  if (request.destination_address ==
      kGlobalAddress) {
    return request.total_size <=
           kFdTpMaxBamMessageBytes;
  }

  return true;
}

bool FdTpTransmitter::observe_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading,
          config_.timestamp_domain) ||
      reading.uncertainty >
          config_.max_timestamp_uncertainty ||
      (has_last_observed_time_ &&
       reading.value < last_observed_time_)) {
    latch_fault();
    return false;
  }

  last_observed_time_ = reading.value;
  has_last_observed_time_ = true;
  return true;
}

bool FdTpTransmitter::set_deadline_after(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration delay,
    time::MonotonicTime& deadline) noexcept {
  const auto maximum =
      (std::numeric_limits<
          time::MonotonicTime::rep>::max)();
  if (reading.value.count() >
          maximum -
              reading.uncertainty.count() ||
      delay.count() < 0) {
    return false;
  }

  const auto upper =
      reading.value.count() +
      reading.uncertainty.count();
  if (upper >
      maximum - delay.count()) {
    return false;
  }

  deadline =
      time::MonotonicTime{
          upper + delay.count()};
  return true;
}

time::MonotonicTime FdTpTransmitter::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <=
      reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() -
      reading.uncertainty.count()};
}

FdTpTransmitter::Session*
FdTpTransmitter::allocate_session(
    const bool broadcast) noexcept {
  if (broadcast) {
    for (std::size_t index = 0U;
         index < bam_sessions_.size();
         ++index) {
      auto& session = bam_sessions_[index];
      if (!session.active) {
        session = {};
        session.active = true;
        session.broadcast = true;
        session.session_number =
            static_cast<std::uint8_t>(
                index);
        return &session;
      }
    }
    return nullptr;
  }

  for (std::size_t index = 0U;
       index < peer_sessions_.size();
       ++index) {
    auto& session = peer_sessions_[index];
    if (!session.active) {
      session = {};
      session.active = true;
      session.broadcast = false;
      session.session_number =
          static_cast<std::uint8_t>(
              index);
      return &session;
    }
  }
  return nullptr;
}

FdTpTransmitter::Session*
FdTpTransmitter::find_peer_session(
    const FdTpCmFrame& cm) noexcept {
  if (cm.key.destination_address !=
      config_.local_address) {
    return nullptr;
  }

  for (auto& session : peer_sessions_) {
    if (session.active &&
        session.session_number ==
            cm.key.session_number &&
        session.request.destination_address ==
            cm.key.source_address &&
        session.request.pgn ==
            cm.transported_pgn) {
      return &session;
    }
  }
  return nullptr;
}

void FdTpTransmitter::handle_control(
    const FdTpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  auto* const session =
      find_peer_session(cm);
  if (session == nullptr) {
    return;
  }

  switch (cm.control) {
    case FdTpControl::cts:
      accept_cts(
          *session,
          cm,
          timestamp);
      break;
    case FdTpControl::end_of_message_ack:
      accept_eom_ack(*session, cm);
      break;
    case FdTpControl::abort:
      accept_abort(*session);
      break;
    case FdTpControl::rts:
    case FdTpControl::end_of_message_status:
    case FdTpControl::bam:
      break;
  }
}

void FdTpTransmitter::accept_cts(
    Session& session,
    const FdTpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (session.phase == Phase::send_data) {
    static_cast<void>(
        queue_abort(
            session,
            FdTpAbortReason::
                cts_while_data_transfer));
    reset_session(session);
    saturating_increment(
        counters_.malformed_control_frames);
    return;
  }

  if (session.phase != Phase::wait_cts ||
      cm.message_size != 0xFFFFFFU ||
      cm.parameter8 != 0U ||
      cm.segment_number == 0U ||
      cm.segment_number >
          session.total_segments ||
      cm.parameter7 >
          config_.max_segments_per_cts) {
    static_cast<void>(
        queue_abort(
            session,
            FdTpAbortReason::resources));
    reset_session(session);
    saturating_increment(
        counters_.malformed_control_frames);
    return;
  }

  if (cm.parameter7 == 0U) {
    if (!set_deadline_after(
            timestamp,
            kHoldTimeout,
            session.deadline)) {
      reset_session(session);
      latch_fault();
    }
    return;
  }

  const auto remaining =
      session.total_segments -
      (cm.segment_number - 1U);
  session.next_segment =
      cm.segment_number;
  session.grant_remaining =
      remaining <
              static_cast<std::uint32_t>(
                  cm.parameter7)
          ? static_cast<std::uint8_t>(
                remaining)
          : cm.parameter7;
  session.phase = Phase::send_data;
  session.next_action =
      lower_bound(timestamp);
}

void FdTpTransmitter::accept_eom_ack(
    Session& session,
    const FdTpCmFrame& cm) noexcept {
  if (session.phase !=
          Phase::wait_eom_ack ||
      cm.message_size !=
          session.request.total_size ||
      cm.segment_number !=
          session.total_segments ||
      cm.parameter7 != 0xFFU ||
      cm.parameter8 != 0xFFU) {
    static_cast<void>(
        queue_abort(
            session,
            FdTpAbortReason::resources));
    reset_session(session);
    saturating_increment(
        counters_.malformed_control_frames);
    return;
  }

  saturating_increment(
      counters_.completed_messages);
  reset_session(session);
}

void FdTpTransmitter::accept_abort(
    Session& session) noexcept {
  reset_session(session);
  saturating_increment(
      counters_.remote_aborts);
}

bool FdTpTransmitter::queue_next_dt(
    Session& session,
    const time::MonotonicClockReading& now) noexcept {
  if (session.source == nullptr ||
      session.next_segment == 0U ||
      session.next_segment >
          session.total_segments) {
    reset_session(session);
    latch_fault();
    return false;
  }

  const auto valid_expected =
      valid_bytes_for_segment(
          session.request.total_size,
          session.next_segment);
  if (valid_expected == 0U) {
    reset_session(session);
    latch_fault();
    return false;
  }

  std::array<std::byte, kFdTpSegmentPayloadBytes>
      data{};
  for (auto& byte : data) {
    byte = std::byte{0xFFU};
  }

  const auto offset =
      (session.next_segment - 1U) *
      static_cast<std::uint32_t>(
          kFdTpSegmentPayloadBytes);
  std::uint8_t valid_bytes = 0U;
  if (!session.source->read(
          offset,
          data,
          valid_bytes) ||
      valid_bytes != valid_expected) {
    static_cast<void>(
        queue_abort(
            session,
            FdTpAbortReason::resources));
    reset_session(session);
    saturating_increment(
        counters_.source_failures);
    return false;
  }

  transport::CanFrame frame{};
  if (!build_fd_tp_dt(
          config_.local_address,
          session.request.destination_address,
          session.session_number,
          session.next_segment,
          data,
          valid_bytes,
          frame) ||
      !enqueue_tx(frame)) {
    reset_session(session);
    return false;
  }

  saturating_increment(
      counters_.dt_frames);
  ++session.next_segment;
  if (!session.broadcast &&
      session.grant_remaining != 0U) {
    --session.grant_remaining;
  }

  if (session.next_segment >
      session.total_segments) {
    session.phase =
        Phase::send_eom_status;
    if (session.broadcast) {
      if (!set_deadline_after(
              now,
              kBamSegmentInterval,
              session.next_action)) {
        reset_session(session);
        latch_fault();
        return false;
      }
    } else {
      session.next_action =
          lower_bound(now);
    }
    return true;
  }

  if (!session.broadcast &&
      session.grant_remaining == 0U) {
    session.phase = Phase::wait_cts;
    if (!set_deadline_after(
            now,
            kResponseTimeout,
            session.deadline)) {
      reset_session(session);
      latch_fault();
      return false;
    }
    return true;
  }

  if (session.broadcast) {
    if (!set_deadline_after(
            now,
            kBamSegmentInterval,
            session.next_action)) {
      reset_session(session);
      latch_fault();
      return false;
    }
  } else {
    session.next_action =
        lower_bound(now);
  }
  return true;
}

bool FdTpTransmitter::queue_eom_status(
    Session& session,
    const time::MonotonicClockReading& now) noexcept {
  transport::CanFrame frame{};
  if (!build_fd_tp_end_of_message_status(
          config_.local_address,
          session.request.destination_address,
          session.session_number,
          session.request.total_size,
          session.request.pgn,
          frame) ||
      !enqueue_tx(frame)) {
    reset_session(session);
    return false;
  }

  saturating_increment(
      counters_.eom_status_frames);
  if (session.broadcast) {
    saturating_increment(
        counters_.completed_messages);
    reset_session(session);
    return true;
  }

  session.phase =
      Phase::wait_eom_ack;
  if (!set_deadline_after(
          now,
          kEomAckTimeout,
          session.deadline)) {
    reset_session(session);
    latch_fault();
    return false;
  }
  return true;
}

bool FdTpTransmitter::queue_abort(
    Session& session,
    const FdTpAbortReason reason) noexcept {
  if (!session.active ||
      session.broadcast) {
    return false;
  }

  transport::CanFrame frame{};
  if (!build_fd_tp_abort(
          config_.local_address,
          session.request.destination_address,
          session.session_number,
          reason,
          session.request.pgn,
          frame)) {
    return false;
  }
  return enqueue_tx(frame);
}

bool FdTpTransmitter::enqueue_tx(
    const transport::CanFrame& frame) noexcept {
  if (tx_count_ >= tx_queue_.size()) {
    status_ =
        FdTpTransmitterStatus::queue_overflow;
    saturating_increment(
        counters_.tx_queue_overflows);
    return false;
  }

  tx_queue_[tx_tail_] = frame;
  tx_tail_ =
      (tx_tail_ + 1U) %
      tx_queue_.size();
  ++tx_count_;
  status_ = FdTpTransmitterStatus::ok;
  return true;
}

void FdTpTransmitter::reset_session(
    Session& session) noexcept {
  session = {};
}

void FdTpTransmitter::latch_fault() noexcept {
  bam_sessions_ = {};
  peer_sessions_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  status_ =
      FdTpTransmitterStatus::protocol_fault;
}

}  // namespace ecu::core::v2::protocol::j1939
