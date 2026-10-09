#include "ecu/core_v2/protocol/j1939/transport_protocol.hpp"

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

std::uint16_t decode_u16(
    const transport::CanFrame& frame,
    const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(
          std::to_integer<std::uint8_t>(frame.payload[offset])) |
      (static_cast<std::uint16_t>(
           std::to_integer<std::uint8_t>(frame.payload[offset + 1U]))
       << 8U));
}

std::uint32_t decode_u24(
    const transport::CanFrame& frame,
    const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(frame.payload[offset])) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[offset + 1U]))
       << 8U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[offset + 2U]))
       << 16U);
}

bool known_control(
    const std::uint8_t value,
    TpControl& control) noexcept {
  switch (value) {
    case static_cast<std::uint8_t>(TpControl::rts):
      control = TpControl::rts;
      return true;
    case static_cast<std::uint8_t>(TpControl::cts):
      control = TpControl::cts;
      return true;
    case static_cast<std::uint8_t>(TpControl::end_of_message_ack):
      control = TpControl::end_of_message_ack;
      return true;
    case static_cast<std::uint8_t>(TpControl::bam):
      control = TpControl::bam;
      return true;
    case static_cast<std::uint8_t>(TpControl::abort):
      control = TpControl::abort;
      return true;
    default:
      return false;
  }
}

bool build_cm(
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::array<std::uint8_t, 8U>& payload,
    transport::CanFrame& frame) noexcept {
  return build_classic_data_frame(
      MessageAddress{
          7U,
          kTpCmPgn,
          source,
          destination},
      payload.data(),
      static_cast<std::uint8_t>(payload.size()),
      frame);
}

std::uint8_t expected_packet_count(
    const std::uint16_t size) noexcept {
  return static_cast<std::uint8_t>(
      (static_cast<std::uint32_t>(size) +
       static_cast<std::uint32_t>(kTpPacketPayloadBytes - 1U)) /
      static_cast<std::uint32_t>(kTpPacketPayloadBytes));
}

bool valid_transport_descriptor(
    const TpCmFrame& cm) noexcept {
  return cm.message_size > 8U &&
         cm.message_size <= kTpMaxMessageBytes &&
         cm.packet_count >= 2U &&
         cm.packet_count ==
             expected_packet_count(cm.message_size) &&
         cm.transported_pgn <= kMaxPgn &&
         cm.transported_pgn != kTpCmPgn &&
         cm.transported_pgn != kTpDtPgn;
}

}  // namespace

bool decode_tp_cm(
    const transport::CanFrame& frame,
    TpCmFrame& value) noexcept {
  IdentifierFields fields{};
  std::uint8_t destination = 0U;
  if (!decode_classic_frame_identifier(frame, fields) ||
      parameter_group_number(fields) != kTpCmPgn ||
      !destination_address(fields, destination) ||
      frame.length != 8U) {
    return false;
  }

  TpControl control{};
  if (!known_control(
          std::to_integer<std::uint8_t>(frame.payload[0]),
          control)) {
    return false;
  }

  value.control = control;
  value.source_address = fields.source_address;
  value.destination_address = destination;
  value.message_size = decode_u16(frame, 1U);
  value.packet_count =
      std::to_integer<std::uint8_t>(frame.payload[3]);
  value.control_parameter =
      std::to_integer<std::uint8_t>(frame.payload[4]);
  value.transported_pgn = decode_u24(frame, 5U);
  return true;
}

bool decode_tp_dt(
    const transport::CanFrame& frame,
    TpDtFrame& value) noexcept {
  IdentifierFields fields{};
  std::uint8_t destination = 0U;
  if (!decode_classic_frame_identifier(frame, fields) ||
      parameter_group_number(fields) != kTpDtPgn ||
      !destination_address(fields, destination) ||
      frame.length != 8U) {
    return false;
  }

  const auto sequence =
      std::to_integer<std::uint8_t>(frame.payload[0]);
  if (sequence == 0U) {
    return false;
  }

  value.source_address = fields.source_address;
  value.destination_address = destination;
  value.sequence_number = sequence;
  for (std::size_t i = 0U; i < value.data.size(); ++i) {
    value.data[i] = frame.payload[i + 1U];
  }
  return true;
}

bool build_tp_cts(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t packets_allowed,
    const std::uint8_t next_packet,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(source_address) ||
      !is_claimable_address(destination_address) ||
      next_packet == 0U ||
      transported_pgn > kMaxPgn) {
    return false;
  }

  const std::array<std::uint8_t, 8U> payload{
      static_cast<std::uint8_t>(TpControl::cts),
      packets_allowed,
      next_packet,
      0xFFU,
      0xFFU,
      static_cast<std::uint8_t>(transported_pgn & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 16U) & 0xFFU)};
  return build_cm(
      source_address,
      destination_address,
      payload,
      frame);
}

bool build_tp_end_of_message_ack(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint16_t message_size,
    const std::uint8_t packet_count,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(source_address) ||
      !is_claimable_address(destination_address) ||
      message_size <= 8U ||
      message_size > kTpMaxMessageBytes ||
      packet_count != expected_packet_count(message_size) ||
      transported_pgn > kMaxPgn) {
    return false;
  }

  const std::array<std::uint8_t, 8U> payload{
      static_cast<std::uint8_t>(
          TpControl::end_of_message_ack),
      static_cast<std::uint8_t>(message_size & 0xFFU),
      static_cast<std::uint8_t>((message_size >> 8U) & 0xFFU),
      packet_count,
      0xFFU,
      static_cast<std::uint8_t>(transported_pgn & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 16U) & 0xFFU)};
  return build_cm(
      source_address,
      destination_address,
      payload,
      frame);
}

bool build_tp_abort(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const TpAbortReason reason,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(source_address) ||
      !is_claimable_address(destination_address) ||
      transported_pgn > kMaxPgn) {
    return false;
  }

  const std::array<std::uint8_t, 8U> payload{
      static_cast<std::uint8_t>(TpControl::abort),
      static_cast<std::uint8_t>(reason),
      0xFFU,
      0xFFU,
      0xFFU,
      static_cast<std::uint8_t>(transported_pgn & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 16U) & 0xFFU)};
  return build_cm(
      source_address,
      destination_address,
      payload,
      frame);
}

bool TpReceiver::configure(
    const TpReceiverConfig& config) noexcept {
  if (!valid_config(config)) {
    return false;
  }

  config_ = config;
  bam_ = {};
  peer_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  message_head_ = 0U;
  message_tail_ = 0U;
  message_count_ = 0U;
  last_observed_time_ = time::MonotonicTime{0};
  has_last_observed_time_ = false;
  configured_ = true;
  status_ = TpReceiverStatus::ok;
  counters_ = {};
  return true;
}

bool TpReceiver::set_local_address(
    const std::uint8_t address) noexcept {
  if (!configured_ ||
      peer_.active ||
      (address != kNullAddress &&
       !is_claimable_address(address))) {
    return false;
  }
  config_.local_address = address;
  return true;
}

void TpReceiver::on_can_frame(
    const transport::ReceivedCanFrame& frame) noexcept {
  if (!configured_ ||
      status_ == TpReceiverStatus::protocol_fault ||
      status_ == TpReceiverStatus::queue_overflow) {
    return;
  }

  if (frame.frame.identifier_format !=
      transport::CanIdentifierFormat::extended_29_bit) {
    return;
  }

  IdentifierFields fields{};
  if (!decode_identifier(frame.frame.identifier, fields)) {
    return;
  }
  const auto pgn = parameter_group_number(fields);
  if (pgn != kTpCmPgn && pgn != kTpDtPgn) {
    return;
  }

  if (!observe_time(frame.timestamp)) {
    return;
  }

  if (pgn == kTpCmPgn) {
    TpCmFrame cm{};
    if (!decode_tp_cm(frame.frame, cm)) {
      saturating_increment(counters_.malformed_frames);
      return;
    }
    saturating_increment(counters_.cm_frames);
    handle_cm(cm, frame.timestamp);
    return;
  }

  TpDtFrame dt{};
  if (!decode_tp_dt(frame.frame, dt)) {
    saturating_increment(counters_.malformed_frames);
    return;
  }
  saturating_increment(counters_.dt_frames);
  handle_dt(dt, frame.timestamp);
}

TpReceiverStatus TpReceiver::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return TpReceiverStatus::invalid_state;
  }
  if (status_ == TpReceiverStatus::protocol_fault ||
      status_ == TpReceiverStatus::queue_overflow) {
    return status_;
  }
  if (!observe_time(now)) {
    return TpReceiverStatus::invalid_time;
  }

  const auto lower = lower_bound(now);
  bool acted = false;

  if (bam_.active && lower > bam_.deadline) {
    reset_session(bam_);
    saturating_increment(counters_.timeouts);
    acted = true;
  }

  if (peer_.active && lower > peer_.deadline) {
    timeout_peer();
    acted = true;
  }

  return acted
             ? status_
             : TpReceiverStatus::no_action;
}

bool TpReceiver::try_take_tx(
    transport::CanFrame& frame) noexcept {
  if (tx_count_ == 0U) {
    return false;
  }
  frame = tx_queue_[tx_head_];
  tx_head_ = (tx_head_ + 1U) % tx_queue_.size();
  --tx_count_;
  return true;
}

bool TpReceiver::try_take_message(
    TpMessage& message) noexcept {
  if (message_count_ == 0U) {
    return false;
  }
  message = message_queue_[message_head_];
  message_head_ =
      (message_head_ + 1U) % message_queue_.size();
  --message_count_;
  return true;
}

TpReceiverStatus TpReceiver::status() const noexcept {
  return status_;
}

TpReceiverCounters TpReceiver::counters() const noexcept {
  return counters_;
}

std::size_t TpReceiver::pending_tx_count() const noexcept {
  return tx_count_;
}

std::size_t TpReceiver::pending_message_count() const noexcept {
  return message_count_;
}

bool TpReceiver::bam_active() const noexcept {
  return bam_.active;
}

bool TpReceiver::peer_active() const noexcept {
  return peer_.active;
}

bool TpReceiver::valid_config(
    const TpReceiverConfig& config) const noexcept {
  return (config.local_address == kNullAddress ||
          is_claimable_address(config.local_address)) &&
         config.max_packets_per_cts != 0U &&
         config.timestamp_domain.valid() &&
         config.max_timestamp_uncertainty.count() >= 0;
}

bool TpReceiver::observe_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading, config_.timestamp_domain) ||
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

bool TpReceiver::set_deadline_after(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration delay,
    time::MonotonicTime& deadline) noexcept {
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (reading.value.count() >
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

time::MonotonicTime TpReceiver::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

void TpReceiver::handle_cm(
    const TpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  switch (cm.control) {
    case TpControl::bam:
      start_bam(cm, timestamp);
      break;
    case TpControl::rts:
      start_peer(cm, timestamp);
      break;
    case TpControl::abort:
      if (peer_.active &&
          cm.source_address == peer_.source_address &&
          cm.destination_address == config_.local_address &&
          cm.transported_pgn == peer_.pgn) {
        reset_session(peer_);
      }
      break;
    case TpControl::cts:
    case TpControl::end_of_message_ack:
      break;
  }
}

void TpReceiver::handle_dt(
    const TpDtFrame& dt,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (bam_.active &&
      dt.destination_address == kGlobalAddress &&
      dt.source_address == bam_.source_address) {
    accept_dt(bam_, dt, timestamp);
    return;
  }

  if (peer_.active &&
      dt.destination_address == config_.local_address &&
      dt.source_address == peer_.source_address) {
    accept_dt(peer_, dt, timestamp);
  }
}

void TpReceiver::start_bam(
    const TpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (cm.destination_address != kGlobalAddress ||
      cm.control_parameter != 0xFFU ||
      !is_claimable_address(cm.source_address) ||
      !valid_transport_descriptor(cm)) {
    saturating_increment(counters_.malformed_frames);
    return;
  }

  if (bam_.active || !has_reserved_message_slot()) {
    saturating_increment(counters_.rejected_sessions);
    return;
  }

  bam_ = {};
  bam_.active = true;
  bam_.broadcast = true;
  bam_.source_address = cm.source_address;
  bam_.destination_address = kGlobalAddress;
  bam_.pgn = cm.transported_pgn;
  bam_.message_size = cm.message_size;
  bam_.packet_count = cm.packet_count;
  bam_.next_sequence = 1U;

  if (!set_deadline_after(
          timestamp, kPacketTimeout, bam_.deadline)) {
    latch_fault();
    return;
  }

  saturating_increment(counters_.bam_sessions_started);
}

void TpReceiver::start_peer(
    const TpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (config_.local_address == kNullAddress ||
      cm.destination_address != config_.local_address ||
      !is_claimable_address(cm.source_address) ||
      !valid_transport_descriptor(cm)) {
    if (cm.destination_address == config_.local_address) {
      saturating_increment(counters_.malformed_frames);
    }
    return;
  }

  if (peer_.active) {
    if (!queue_abort(
            cm.source_address,
            TpAbortReason::already_in_session,
            cm.transported_pgn)) {
      return;
    }
    saturating_increment(counters_.rejected_sessions);
    return;
  }

  if (!has_reserved_message_slot()) {
    if (!queue_abort(
            cm.source_address,
            TpAbortReason::resources,
            cm.transported_pgn)) {
      return;
    }
    saturating_increment(counters_.rejected_sessions);
    return;
  }

  peer_ = {};
  peer_.active = true;
  peer_.broadcast = false;
  peer_.source_address = cm.source_address;
  peer_.destination_address = config_.local_address;
  peer_.pgn = cm.transported_pgn;
  peer_.message_size = cm.message_size;
  peer_.packet_count = cm.packet_count;
  peer_.next_sequence = 1U;

  const auto grant =
      cm.packet_count < config_.max_packets_per_cts
          ? cm.packet_count
          : config_.max_packets_per_cts;
  peer_.granted_packets_remaining = grant;

  if (!queue_cts(peer_, grant, 1U) ||
      !set_deadline_after(
          timestamp, kCtsDataTimeout, peer_.deadline)) {
    if (status_ == TpReceiverStatus::ok) {
      latch_fault();
    }
    return;
  }

  saturating_increment(counters_.peer_sessions_started);
}

void TpReceiver::accept_dt(
    RxSession& session,
    const TpDtFrame& dt,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (dt.sequence_number != session.next_sequence ||
      (!session.broadcast &&
       session.granted_packets_remaining == 0U)) {
    saturating_increment(counters_.malformed_frames);
    reset_session(session);
    return;
  }

  const auto remaining =
      static_cast<std::size_t>(session.message_size) -
      static_cast<std::size_t>(session.bytes_received);
  const auto copy_count =
      remaining < kTpPacketPayloadBytes
          ? remaining
          : kTpPacketPayloadBytes;

  for (std::size_t i = 0U; i < copy_count; ++i) {
    session.data[
        static_cast<std::size_t>(session.bytes_received) + i] =
        dt.data[i];
  }

  if (copy_count < kTpPacketPayloadBytes) {
    for (std::size_t i = copy_count;
         i < kTpPacketPayloadBytes;
         ++i) {
      if (dt.data[i] != static_cast<std::byte>(0xFFU)) {
        saturating_increment(counters_.malformed_frames);
        reset_session(session);
        return;
      }
    }
  }

  session.bytes_received = static_cast<std::uint16_t>(
      static_cast<std::size_t>(session.bytes_received) +
      copy_count);
  ++session.next_sequence;
  if (!session.broadcast) {
    --session.granted_packets_remaining;
  }

  const bool complete =
      session.bytes_received == session.message_size &&
      static_cast<std::uint16_t>(dt.sequence_number) ==
          session.packet_count;
  if (complete) {
    if (!session.broadcast && !queue_eom_ack(session)) {
      reset_session(session);
      return;
    }
    complete_session(session);
    return;
  }

  if (session.bytes_received >= session.message_size ||
      dt.sequence_number >= session.packet_count) {
    saturating_increment(counters_.malformed_frames);
    reset_session(session);
    return;
  }

  if (!session.broadcast &&
      session.granted_packets_remaining == 0U) {
    const auto packets_left = static_cast<std::uint8_t>(
        session.packet_count -
        static_cast<std::uint8_t>(session.next_sequence - 1U));
    const auto grant =
        packets_left < config_.max_packets_per_cts
            ? packets_left
            : config_.max_packets_per_cts;
    session.granted_packets_remaining = grant;
    if (!queue_cts(
            session,
            grant,
            session.next_sequence) ||
        !set_deadline_after(
            timestamp,
            kCtsDataTimeout,
            session.deadline)) {
      reset_session(session);
    }
    return;
  }

  if (!set_deadline_after(
          timestamp, kPacketTimeout, session.deadline)) {
    latch_fault();
  }
}

void TpReceiver::complete_session(
    RxSession& session) noexcept {
  if (!enqueue_message(session)) {
    latch_fault();
    return;
  }
  saturating_increment(counters_.completed_messages);
  reset_session(session);
}

void TpReceiver::reset_session(
    RxSession& session) noexcept {
  session = {};
}

void TpReceiver::timeout_peer() noexcept {
  if (!peer_.active) {
    return;
  }
  const auto destination = peer_.source_address;
  const auto pgn = peer_.pgn;
  reset_session(peer_);
  saturating_increment(counters_.timeouts);
  (void)queue_abort(
      destination,
      TpAbortReason::timeout,
      pgn);
}

bool TpReceiver::queue_cts(
    const RxSession& session,
    const std::uint8_t packets_allowed,
    const std::uint8_t next_packet) noexcept {
  transport::CanFrame frame{};
  if (!build_tp_cts(
          config_.local_address,
          session.source_address,
          packets_allowed,
          next_packet,
          session.pgn,
          frame)) {
    latch_fault();
    return false;
  }
  return enqueue_tx(frame);
}

bool TpReceiver::queue_eom_ack(
    const RxSession& session) noexcept {
  transport::CanFrame frame{};
  if (!build_tp_end_of_message_ack(
          config_.local_address,
          session.source_address,
          session.message_size,
          session.packet_count,
          session.pgn,
          frame)) {
    latch_fault();
    return false;
  }
  return enqueue_tx(frame);
}

bool TpReceiver::queue_abort(
    const std::uint8_t destination,
    const TpAbortReason reason,
    const std::uint32_t pgn) noexcept {
  if (config_.local_address == kNullAddress) {
    return false;
  }
  transport::CanFrame frame{};
  if (!build_tp_abort(
          config_.local_address,
          destination,
          reason,
          pgn,
          frame)) {
    latch_fault();
    return false;
  }
  return enqueue_tx(frame);
}

bool TpReceiver::enqueue_tx(
    const transport::CanFrame& frame) noexcept {
  if (tx_count_ >= tx_queue_.size()) {
    status_ = TpReceiverStatus::queue_overflow;
    saturating_increment(counters_.tx_queue_overflows);
    reset_session(bam_);
    reset_session(peer_);
    return false;
  }

  tx_queue_[tx_tail_] = frame;
  tx_tail_ = (tx_tail_ + 1U) % tx_queue_.size();
  ++tx_count_;
  status_ = TpReceiverStatus::ok;
  return true;
}

bool TpReceiver::enqueue_message(
    const RxSession& session) noexcept {
  if (message_count_ >= message_queue_.size()) {
    return false;
  }

  auto& message = message_queue_[message_tail_];
  message = {};
  message.pgn = session.pgn;
  message.source_address = session.source_address;
  message.destination_address = session.destination_address;
  message.size = session.message_size;
  message.broadcast = session.broadcast;
  for (std::size_t i = 0U;
       i < session.message_size;
       ++i) {
    message.data[i] = session.data[i];
  }

  message_tail_ =
      (message_tail_ + 1U) % message_queue_.size();
  ++message_count_;
  return true;
}

bool TpReceiver::has_reserved_message_slot() const noexcept {
  const std::size_t reserved =
      message_count_ +
      (bam_.active ? 1U : 0U) +
      (peer_.active ? 1U : 0U);
  return reserved < message_queue_.size();
}

void TpReceiver::latch_fault() noexcept {
  status_ = TpReceiverStatus::protocol_fault;
  reset_session(bam_);
  reset_session(peer_);
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
}

bool build_tp_bam(
    const std::uint8_t source_address,
    const std::uint16_t message_size,
    const std::uint8_t packet_count,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(source_address) ||
      message_size <= 8U ||
      message_size > kTpMaxMessageBytes ||
      packet_count != expected_packet_count(message_size) ||
      transported_pgn > kMaxPgn) {
    return false;
  }

  const std::array<std::uint8_t, 8U> payload{
      static_cast<std::uint8_t>(TpControl::bam),
      static_cast<std::uint8_t>(message_size & 0xFFU),
      static_cast<std::uint8_t>((message_size >> 8U) & 0xFFU),
      packet_count,
      0xFFU,
      static_cast<std::uint8_t>(transported_pgn & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 16U) & 0xFFU)};
  return build_cm(
      source_address,
      kGlobalAddress,
      payload,
      frame);
}

bool build_tp_rts(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint16_t message_size,
    const std::uint8_t packet_count,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(source_address) ||
      !is_claimable_address(destination_address) ||
      message_size <= 8U ||
      message_size > kTpMaxMessageBytes ||
      packet_count != expected_packet_count(message_size) ||
      transported_pgn > kMaxPgn) {
    return false;
  }

  const std::array<std::uint8_t, 8U> payload{
      static_cast<std::uint8_t>(TpControl::rts),
      static_cast<std::uint8_t>(message_size & 0xFFU),
      static_cast<std::uint8_t>((message_size >> 8U) & 0xFFU),
      packet_count,
      0xFFU,
      static_cast<std::uint8_t>(transported_pgn & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((transported_pgn >> 16U) & 0xFFU)};
  return build_cm(
      source_address,
      destination_address,
      payload,
      frame);
}

bool build_tp_dt(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t sequence_number,
    const std::array<std::byte, kTpPacketPayloadBytes>& data,
    transport::CanFrame& frame) noexcept {
  if (!is_claimable_address(source_address) ||
      (destination_address != kGlobalAddress &&
       !is_claimable_address(destination_address)) ||
      sequence_number == 0U) {
    return false;
  }

  std::uint8_t payload[8] = {
      sequence_number,
      0xFFU,
      0xFFU,
      0xFFU,
      0xFFU,
      0xFFU,
      0xFFU,
      0xFFU};
  for (std::size_t i = 0U; i < data.size(); ++i) {
    payload[i + 1U] =
        std::to_integer<std::uint8_t>(data[i]);
  }

  return build_classic_data_frame(
      MessageAddress{
          7U,
          kTpDtPgn,
          source_address,
          destination_address},
      payload,
      8U,
      frame);
}

bool TpTransmitter::configure(
    const TpTransmitterConfig& config) noexcept {
  if (!valid_config(config)) {
    return false;
  }

  config_ = config;
  bam_ = {};
  peer_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  last_observed_time_ = time::MonotonicTime{0};
  has_last_observed_time_ = false;
  configured_ = true;
  status_ = TpTransmitterStatus::ok;
  counters_ = {};
  return true;
}

bool TpTransmitter::set_local_address(
    const std::uint8_t address) noexcept {
  if (!configured_ ||
      bam_.active ||
      peer_.active ||
      (address != kNullAddress &&
       !is_claimable_address(address))) {
    return false;
  }
  config_.local_address = address;
  return true;
}

TpTransmitterStatus TpTransmitter::submit(
    const TpTransmitMessage& message,
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_ ||
      status_ == TpTransmitterStatus::protocol_fault ||
      status_ == TpTransmitterStatus::queue_overflow ||
      config_.local_address == kNullAddress) {
    return TpTransmitterStatus::invalid_state;
  }
  if (!valid_message(message)) {
    return TpTransmitterStatus::invalid_argument;
  }
  if (!observe_time(now)) {
    return TpTransmitterStatus::invalid_time;
  }

  if (message.destination_address == kGlobalAddress) {
    return start_bam(message, now);
  }
  return start_peer(message, now);
}

void TpTransmitter::on_can_frame(
    const transport::ReceivedCanFrame& frame) noexcept {
  if (!configured_ ||
      !peer_.active ||
      status_ == TpTransmitterStatus::protocol_fault ||
      status_ == TpTransmitterStatus::queue_overflow) {
    return;
  }

  TpCmFrame cm{};
  if (!decode_tp_cm(frame.frame, cm)) {
    return;
  }
  if (cm.source_address != peer_.destination_address ||
      cm.destination_address != config_.local_address ||
      cm.transported_pgn != peer_.pgn) {
    return;
  }
  if (!observe_time(frame.timestamp)) {
    return;
  }

  handle_peer_control(frame.frame, cm, frame.timestamp);
}

TpTransmitterStatus TpTransmitter::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return TpTransmitterStatus::invalid_state;
  }
  if (status_ == TpTransmitterStatus::protocol_fault ||
      status_ == TpTransmitterStatus::queue_overflow) {
    return status_;
  }
  if (!observe_time(now)) {
    return TpTransmitterStatus::invalid_time;
  }

  const auto lower = lower_bound(now);
  bool acted = false;

  if (bam_.active) {
    if (lower > bam_.deadline) {
      reset_session(bam_);
      saturating_increment(counters_.timeouts);
      saturating_increment(counters_.aborted_messages);
      acted = true;
    } else if (lower >= bam_.next_packet_due) {
      if (!queue_next_dt(bam_, now)) {
        return status_;
      }
      acted = true;
    }
  }

  if (peer_.active) {
    if (peer_.phase == SessionPhase::peer_send_block) {
      if (lower > peer_.deadline) {
        if (!queue_timeout_abort(peer_)) {
          return status_;
        }
        saturating_increment(counters_.timeouts);
        saturating_increment(counters_.aborted_messages);
        reset_session(peer_);
        acted = true;
      } else {
        if (!queue_next_dt(peer_, now)) {
          return status_;
        }
        acted = true;
      }
    } else if (
        (peer_.phase == SessionPhase::peer_wait_cts ||
         peer_.phase == SessionPhase::peer_wait_eom) &&
        lower > peer_.deadline) {
      if (!queue_timeout_abort(peer_)) {
        return status_;
      }
      saturating_increment(counters_.timeouts);
      saturating_increment(counters_.aborted_messages);
      reset_session(peer_);
      acted = true;
    }
  }

  return acted
             ? TpTransmitterStatus::ok
             : TpTransmitterStatus::no_action;
}

bool TpTransmitter::try_take_tx(
    transport::CanFrame& frame) noexcept {
  if (tx_count_ == 0U) {
    return false;
  }

  frame = tx_queue_[tx_head_];
  tx_head_ = (tx_head_ + 1U) % tx_queue_.size();
  --tx_count_;
  return true;
}

bool TpTransmitter::bam_active() const noexcept {
  return bam_.active;
}

bool TpTransmitter::peer_active() const noexcept {
  return peer_.active;
}

TpTransmitterStatus TpTransmitter::status() const noexcept {
  return status_;
}

TpTransmitterCounters TpTransmitter::counters() const noexcept {
  return counters_;
}

std::size_t TpTransmitter::pending_tx_count() const noexcept {
  return tx_count_;
}

bool TpTransmitter::valid_config(
    const TpTransmitterConfig& config) const noexcept {
  return (config.local_address == kNullAddress ||
          is_claimable_address(config.local_address)) &&
         config.timestamp_domain.valid() &&
         config.max_timestamp_uncertainty.count() >= 0 &&
         config.bam_packet_interval >= kMinBamPacketInterval &&
         config.bam_packet_interval <= kMaxBamPacketInterval;
}

bool TpTransmitter::valid_message(
    const TpTransmitMessage& message) const noexcept {
  return message.size > 8U &&
         message.size <= kTpMaxMessageBytes &&
         message.pgn <= kMaxPgn &&
         message.pgn != kTpCmPgn &&
         message.pgn != kTpDtPgn &&
         (message.destination_address == kGlobalAddress ||
          is_claimable_address(message.destination_address));
}

bool TpTransmitter::observe_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading, config_.timestamp_domain) ||
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

bool TpTransmitter::set_deadline_after(
    const time::MonotonicClockReading& reading,
    const time::MonotonicDuration delay,
    time::MonotonicTime& deadline) noexcept {
  const auto maximum =
      (std::numeric_limits<time::MonotonicTime::rep>::max)();
  if (reading.value.count() >
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

time::MonotonicTime TpTransmitter::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

TpTransmitterStatus TpTransmitter::start_bam(
    const TpTransmitMessage& message,
    const time::MonotonicClockReading& now) noexcept {
  if (bam_.active) {
    return TpTransmitterStatus::busy;
  }

  bam_ = {};
  bam_.active = true;
  bam_.broadcast = true;
  bam_.phase = SessionPhase::bam_wait_packet;
  bam_.destination_address = kGlobalAddress;
  bam_.pgn = message.pgn;
  bam_.size = message.size;
  bam_.packet_count = expected_packet_count(message.size);
  bam_.next_sequence = 1U;
  for (std::size_t i = 0U; i < message.size; ++i) {
    bam_.data[i] = message.data[i];
  }

  transport::CanFrame frame{};
  if (!build_tp_bam(
          config_.local_address,
          bam_.size,
          bam_.packet_count,
          bam_.pgn,
          frame) ||
      !enqueue_tx(frame) ||
      !set_deadline_after(
          now,
          config_.bam_packet_interval,
          bam_.next_packet_due) ||
      !set_deadline_after(
          now,
          kMaxBamPacketInterval,
          bam_.deadline)) {
    if (status_ == TpTransmitterStatus::ok) {
      latch_fault();
    }
    return status_;
  }

  saturating_increment(counters_.bam_messages_started);
  return TpTransmitterStatus::ok;
}

TpTransmitterStatus TpTransmitter::start_peer(
    const TpTransmitMessage& message,
    const time::MonotonicClockReading& now) noexcept {
  if (peer_.active) {
    return TpTransmitterStatus::busy;
  }

  peer_ = {};
  peer_.active = true;
  peer_.broadcast = false;
  peer_.phase = SessionPhase::peer_wait_cts;
  peer_.destination_address = message.destination_address;
  peer_.pgn = message.pgn;
  peer_.size = message.size;
  peer_.packet_count = expected_packet_count(message.size);
  peer_.next_sequence = 1U;
  for (std::size_t i = 0U; i < message.size; ++i) {
    peer_.data[i] = message.data[i];
  }

  transport::CanFrame frame{};
  if (!build_tp_rts(
          config_.local_address,
          peer_.destination_address,
          peer_.size,
          peer_.packet_count,
          peer_.pgn,
          frame) ||
      !enqueue_tx(frame) ||
      !set_deadline_after(
          now,
          kResponseTimeout,
          peer_.deadline)) {
    if (status_ == TpTransmitterStatus::ok) {
      latch_fault();
    }
    return status_;
  }

  saturating_increment(counters_.peer_messages_started);
  return TpTransmitterStatus::ok;
}

void TpTransmitter::handle_peer_control(
    const transport::CanFrame& frame,
    const TpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  switch (cm.control) {
    case TpControl::cts: {
      if (peer_.phase != SessionPhase::peer_wait_cts) {
        saturating_increment(counters_.malformed_control_frames);
        return;
      }

      const auto packets_allowed =
          std::to_integer<std::uint8_t>(frame.payload[1]);
      const auto next_packet =
          std::to_integer<std::uint8_t>(frame.payload[2]);
      if (next_packet == 0U ||
          next_packet > peer_.packet_count) {
        saturating_increment(counters_.malformed_control_frames);
        return;
      }

      if (packets_allowed == 0U) {
        if (!set_deadline_after(
                timestamp,
                kHoldTimeout,
                peer_.deadline)) {
          latch_fault();
        }
        return;
      }

      const auto remaining = static_cast<std::uint8_t>(
          peer_.packet_count - next_packet + 1U);
      peer_.next_sequence = next_packet;
      peer_.block_remaining =
          packets_allowed < remaining
              ? packets_allowed
              : remaining;
      peer_.phase = SessionPhase::peer_send_block;
      if (!set_deadline_after(
              timestamp,
              kPacketProgressTimeout,
              peer_.deadline)) {
        latch_fault();
      }
      break;
    }

    case TpControl::end_of_message_ack:
      if (peer_.phase != SessionPhase::peer_wait_eom ||
          cm.message_size != peer_.size ||
          cm.packet_count != peer_.packet_count) {
        saturating_increment(counters_.malformed_control_frames);
        return;
      }
      complete_session(peer_);
      break;

    case TpControl::abort:
      reset_session(peer_);
      saturating_increment(counters_.aborted_messages);
      break;

    case TpControl::rts:
    case TpControl::bam:
      break;
  }
}

bool TpTransmitter::queue_next_dt(
    TxSession& session,
    const time::MonotonicClockReading& now) noexcept {
  if (!session.active ||
      session.next_sequence == 0U ||
      session.next_sequence > session.packet_count) {
    latch_fault();
    return false;
  }

  const auto offset =
      (static_cast<std::size_t>(session.next_sequence) - 1U) *
      kTpPacketPayloadBytes;
  std::array<std::byte, kTpPacketPayloadBytes> data{};
  for (auto& byte : data) {
    byte = static_cast<std::byte>(0xFFU);
  }

  const auto remaining =
      static_cast<std::size_t>(session.size) - offset;
  const auto count =
      remaining < kTpPacketPayloadBytes
          ? remaining
          : kTpPacketPayloadBytes;
  for (std::size_t i = 0U; i < count; ++i) {
    data[i] = session.data[offset + i];
  }

  transport::CanFrame frame{};
  if (!build_tp_dt(
          config_.local_address,
          session.destination_address,
          session.next_sequence,
          data,
          frame) ||
      !enqueue_tx(frame)) {
    return false;
  }

  saturating_increment(counters_.dt_packets_queued);
  const bool last =
      session.next_sequence == session.packet_count;
  ++session.next_sequence;

  if (session.broadcast) {
    if (last) {
      complete_session(session);
      return true;
    }
    if (!set_deadline_after(
            now,
            config_.bam_packet_interval,
            session.next_packet_due) ||
        !set_deadline_after(
            now,
            kMaxBamPacketInterval,
            session.deadline)) {
      latch_fault();
      return false;
    }
    return true;
  }

  if (session.block_remaining == 0U) {
    latch_fault();
    return false;
  }
  --session.block_remaining;

  if (last) {
    session.phase = SessionPhase::peer_wait_eom;
    if (!set_deadline_after(
            now,
            kPeerCompletionTimeout,
            session.deadline)) {
      latch_fault();
      return false;
    }
    return true;
  }

  if (session.block_remaining == 0U) {
    session.phase = SessionPhase::peer_wait_cts;
    if (!set_deadline_after(
            now,
            kPeerCompletionTimeout,
            session.deadline)) {
      latch_fault();
      return false;
    }
    return true;
  }

  session.phase = SessionPhase::peer_send_block;
  if (!set_deadline_after(
          now,
          kPacketProgressTimeout,
          session.deadline)) {
    latch_fault();
    return false;
  }
  return true;
}

bool TpTransmitter::queue_timeout_abort(
    TxSession& session) noexcept {
  if (!session.active || session.broadcast) {
    return false;
  }

  transport::CanFrame frame{};
  if (!build_tp_abort(
          config_.local_address,
          session.destination_address,
          TpAbortReason::timeout,
          session.pgn,
          frame)) {
    latch_fault();
    return false;
  }
  return enqueue_tx(frame);
}

bool TpTransmitter::enqueue_tx(
    const transport::CanFrame& frame) noexcept {
  if (tx_count_ >= tx_queue_.size()) {
    status_ = TpTransmitterStatus::queue_overflow;
    saturating_increment(counters_.tx_queue_overflows);
    reset_session(bam_);
    reset_session(peer_);
    return false;
  }

  tx_queue_[tx_tail_] = frame;
  tx_tail_ = (tx_tail_ + 1U) % tx_queue_.size();
  ++tx_count_;
  status_ = TpTransmitterStatus::ok;
  return true;
}

void TpTransmitter::complete_session(
    TxSession& session) noexcept {
  saturating_increment(counters_.completed_messages);
  reset_session(session);
}

void TpTransmitter::reset_session(
    TxSession& session) noexcept {
  session = {};
}

void TpTransmitter::latch_fault() noexcept {
  status_ = TpTransmitterStatus::protocol_fault;
  reset_session(bam_);
  reset_session(peer_);
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
}

}  // namespace ecu::core::v2::protocol::j1939
