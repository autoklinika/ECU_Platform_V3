#include "ecu/core_v2/protocol/isobus/extended_transport.hpp"

#include <limits>

namespace ecu::core::v2::protocol::isobus {

namespace {

using j1939::IdentifierFields;
using j1939::MessageAddress;

void saturating_increment(std::uint32_t& value) noexcept {
  const auto maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  if (value != maximum) {
    ++value;
  }
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

std::uint32_t decode_u32(
    const transport::CanFrame& frame,
    const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(frame.payload[offset])) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[offset + 1U]))
       << 8U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[offset + 2U]))
       << 16U) |
      (static_cast<std::uint32_t>(
           std::to_integer<std::uint8_t>(frame.payload[offset + 3U]))
       << 24U);
}

bool known_control(
    const std::uint8_t raw,
    EtpControl& control) noexcept {
  switch (raw) {
    case static_cast<std::uint8_t>(EtpControl::rts):
      control = EtpControl::rts;
      return true;
    case static_cast<std::uint8_t>(EtpControl::cts):
      control = EtpControl::cts;
      return true;
    case static_cast<std::uint8_t>(EtpControl::dpo):
      control = EtpControl::dpo;
      return true;
    case static_cast<std::uint8_t>(EtpControl::eoma):
      control = EtpControl::eoma;
      return true;
    case static_cast<std::uint8_t>(EtpControl::abort):
      control = EtpControl::abort;
      return true;
    default:
      return false;
  }
}

bool valid_transported_pgn(const std::uint32_t pgn) noexcept {
  return pgn <= j1939::kMaxPgn &&
         pgn != kEtpCmPgn &&
         pgn != kEtpDtPgn;
}

bool valid_etp_size(const std::uint32_t size) noexcept {
  return size >= kEtpMinMessageBytes &&
         size <= kEtpMaxMessageBytes &&
         etp_packet_count(size) <= kEtpMaxPacketCount;
}

bool build_cm(
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::array<std::uint8_t, 8U>& payload,
    transport::CanFrame& frame) noexcept {
  return j1939::build_classic_data_frame(
      MessageAddress{
          7U,
          kEtpCmPgn,
          source,
          destination},
      payload.data(),
      static_cast<std::uint8_t>(payload.size()),
      frame);
}

void encode_pgn(
    const std::uint32_t pgn,
    std::array<std::uint8_t, 8U>& payload) noexcept {
  payload[5] = static_cast<std::uint8_t>(pgn & 0xFFU);
  payload[6] = static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU);
  payload[7] = static_cast<std::uint8_t>((pgn >> 16U) & 0xFFU);
}

void encode_u24(
    const std::uint32_t value,
    std::array<std::uint8_t, 8U>& payload,
    const std::size_t offset) noexcept {
  payload[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  payload[offset + 1U] =
      static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  payload[offset + 2U] =
      static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
}

void encode_u32(
    const std::uint32_t value,
    std::array<std::uint8_t, 8U>& payload,
    const std::size_t offset) noexcept {
  payload[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  payload[offset + 1U] =
      static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  payload[offset + 2U] =
      static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  payload[offset + 3U] =
      static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

}  // namespace

bool decode_etp_cm(
    const transport::CanFrame& frame,
    EtpCmFrame& value) noexcept {
  IdentifierFields fields{};
  std::uint8_t destination = 0U;
  if (!j1939::decode_classic_frame_identifier(frame, fields) ||
      j1939::parameter_group_number(fields) != kEtpCmPgn ||
      !j1939::destination_address(fields, destination) ||
      frame.length != 8U) {
    return false;
  }

  EtpControl control{};
  if (!known_control(
          std::to_integer<std::uint8_t>(frame.payload[0]),
          control)) {
    return false;
  }

  value = {};
  value.control = control;
  value.source_address = fields.source_address;
  value.destination_address = destination;
  value.transported_pgn = decode_u24(frame, 5U);

  switch (control) {
    case EtpControl::rts:
    case EtpControl::eoma:
      value.message_size = decode_u32(frame, 1U);
      break;
    case EtpControl::cts:
      value.packets_allowed =
          std::to_integer<std::uint8_t>(frame.payload[1]);
      value.next_packet = decode_u24(frame, 2U);
      break;
    case EtpControl::dpo:
      value.block_count =
          std::to_integer<std::uint8_t>(frame.payload[1]);
      value.packet_offset = decode_u24(frame, 2U);
      break;
    case EtpControl::abort:
      value.abort_reason =
          std::to_integer<std::uint8_t>(frame.payload[1]);
      break;
  }
  return true;
}

bool decode_etp_dt(
    const transport::CanFrame& frame,
    EtpDtFrame& value) noexcept {
  IdentifierFields fields{};
  std::uint8_t destination = 0U;
  if (!j1939::decode_classic_frame_identifier(frame, fields) ||
      j1939::parameter_group_number(fields) != kEtpDtPgn ||
      !j1939::destination_address(fields, destination) ||
      frame.length != 8U) {
    return false;
  }

  const auto sequence =
      std::to_integer<std::uint8_t>(frame.payload[0]);
  if (sequence == 0U) {
    return false;
  }

  value = {};
  value.source_address = fields.source_address;
  value.destination_address = destination;
  value.sequence_number = sequence;
  for (std::size_t i = 0U; i < value.data.size(); ++i) {
    value.data[i] = frame.payload[i + 1U];
  }
  return true;
}

bool build_etp_rts(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint32_t message_size,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!j1939::is_claimable_address(source_address) ||
      !j1939::is_claimable_address(destination_address) ||
      !valid_etp_size(message_size) ||
      !valid_transported_pgn(transported_pgn)) {
    return false;
  }

  std::array<std::uint8_t, 8U> payload{};
  payload[0] = static_cast<std::uint8_t>(EtpControl::rts);
  encode_u32(message_size, payload, 1U);
  encode_pgn(transported_pgn, payload);
  return build_cm(
      source_address, destination_address, payload, frame);
}

bool build_etp_cts(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t packets_allowed,
    const std::uint32_t next_packet,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!j1939::is_claimable_address(source_address) ||
      !j1939::is_claimable_address(destination_address) ||
      next_packet == 0U ||
      next_packet > kEtpMaxPacketCount ||
      !valid_transported_pgn(transported_pgn)) {
    return false;
  }

  std::array<std::uint8_t, 8U> payload{};
  payload[0] = static_cast<std::uint8_t>(EtpControl::cts);
  payload[1] = packets_allowed;
  encode_u24(next_packet, payload, 2U);
  encode_pgn(transported_pgn, payload);
  return build_cm(
      source_address, destination_address, payload, frame);
}

bool build_etp_dpo(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t block_count,
    const std::uint32_t packet_offset,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!j1939::is_claimable_address(source_address) ||
      !j1939::is_claimable_address(destination_address) ||
      block_count == 0U ||
      packet_offset >= kEtpMaxPacketCount ||
      packet_offset >
          kEtpMaxPacketCount -
              static_cast<std::uint32_t>(block_count) ||
      !valid_transported_pgn(transported_pgn)) {
    return false;
  }

  std::array<std::uint8_t, 8U> payload{};
  payload[0] = static_cast<std::uint8_t>(EtpControl::dpo);
  payload[1] = block_count;
  encode_u24(packet_offset, payload, 2U);
  encode_pgn(transported_pgn, payload);
  return build_cm(
      source_address, destination_address, payload, frame);
}

bool build_etp_eoma(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint32_t message_size,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!j1939::is_claimable_address(source_address) ||
      !j1939::is_claimable_address(destination_address) ||
      !valid_etp_size(message_size) ||
      !valid_transported_pgn(transported_pgn)) {
    return false;
  }

  std::array<std::uint8_t, 8U> payload{};
  payload[0] = static_cast<std::uint8_t>(EtpControl::eoma);
  encode_u32(message_size, payload, 1U);
  encode_pgn(transported_pgn, payload);
  return build_cm(
      source_address, destination_address, payload, frame);
}

bool build_etp_abort(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const EtpAbortReason reason,
    const std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept {
  if (!j1939::is_claimable_address(source_address) ||
      !j1939::is_claimable_address(destination_address) ||
      !valid_transported_pgn(transported_pgn)) {
    return false;
  }

  std::array<std::uint8_t, 8U> payload{};
  payload[0] = static_cast<std::uint8_t>(EtpControl::abort);
  payload[1] = static_cast<std::uint8_t>(reason);
  payload[2] = 0xFFU;
  payload[3] = 0xFFU;
  payload[4] = 0xFFU;
  encode_pgn(transported_pgn, payload);
  return build_cm(
      source_address, destination_address, payload, frame);
}

bool build_etp_dt(
    const std::uint8_t source_address,
    const std::uint8_t destination_address,
    const std::uint8_t sequence_number,
    const std::array<std::byte, kEtpPacketPayloadBytes>& data,
    transport::CanFrame& frame) noexcept {
  if (!j1939::is_claimable_address(source_address) ||
      !j1939::is_claimable_address(destination_address) ||
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

  return j1939::build_classic_data_frame(
      MessageAddress{
          7U,
          kEtpDtPgn,
          source_address,
          destination_address},
      payload,
      8U,
      frame);
}

bool EtpReceiver::configure(
    const EtpReceiverConfig& config,
    IEtpReceiveSink& sink) noexcept {
  if (!valid_config(config) || session_.active) {
    return false;
  }

  config_ = config;
  sink_ = &sink;
  session_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  last_observed_time_ = time::MonotonicTime{0};
  has_last_observed_time_ = false;
  configured_ = true;
  status_ = EtpReceiverStatus::ok;
  counters_ = {};
  return true;
}

bool EtpReceiver::set_local_address(
    const std::uint8_t address) noexcept {
  if (!configured_ ||
      session_.active ||
      (address != j1939::kNullAddress &&
       !j1939::is_claimable_address(address))) {
    return false;
  }
  config_.local_address = address;
  return true;
}

void EtpReceiver::on_can_frame(
    const transport::ReceivedCanFrame& frame) noexcept {
  if (!configured_ ||
      status_ == EtpReceiverStatus::protocol_fault ||
      status_ == EtpReceiverStatus::queue_overflow) {
    return;
  }

  IdentifierFields fields{};
  if (!j1939::decode_classic_frame_identifier(
          frame.frame, fields)) {
    return;
  }

  const auto pgn = j1939::parameter_group_number(fields);
  if (pgn != kEtpCmPgn && pgn != kEtpDtPgn) {
    return;
  }

  if (!observe_time(frame.timestamp)) {
    return;
  }

  if (pgn == kEtpCmPgn) {
    EtpCmFrame cm{};
    if (!decode_etp_cm(frame.frame, cm)) {
      saturating_increment(counters_.malformed_frames);
      return;
    }
    saturating_increment(counters_.cm_frames);
    handle_cm(frame.frame, cm, frame.timestamp);
    return;
  }

  EtpDtFrame dt{};
  if (!decode_etp_dt(frame.frame, dt)) {
    saturating_increment(counters_.malformed_frames);
    return;
  }
  saturating_increment(counters_.dt_frames);
  handle_dt(dt, frame.timestamp);
}

EtpReceiverStatus EtpReceiver::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return EtpReceiverStatus::invalid_state;
  }
  if (status_ == EtpReceiverStatus::protocol_fault ||
      status_ == EtpReceiverStatus::queue_overflow) {
    return status_;
  }
  if (!observe_time(now)) {
    return EtpReceiverStatus::invalid_time;
  }

  if (session_.active &&
      lower_bound(now) > session_.deadline) {
    const auto source = session_.source_address;
    const auto pgn = session_.pgn;
    if (!queue_abort(source, EtpAbortReason::timeout, pgn)) {
      abort_session(true);
      return status_;
    }
    saturating_increment(counters_.timeouts);
    abort_session(true);
    return EtpReceiverStatus::ok;
  }

  return EtpReceiverStatus::no_action;
}

bool EtpReceiver::try_take_tx(
    transport::CanFrame& frame) noexcept {
  if (tx_count_ == 0U) {
    return false;
  }
  frame = tx_queue_[tx_head_];
  tx_head_ = (tx_head_ + 1U) % tx_queue_.size();
  --tx_count_;
  return true;
}

EtpReceiverStatus EtpReceiver::status() const noexcept {
  return status_;
}

EtpReceiverCounters EtpReceiver::counters() const noexcept {
  return counters_;
}

bool EtpReceiver::active() const noexcept {
  return session_.active;
}

std::size_t EtpReceiver::pending_tx_count() const noexcept {
  return tx_count_;
}

bool EtpReceiver::valid_config(
    const EtpReceiverConfig& config) const noexcept {
  return (config.local_address == j1939::kNullAddress ||
          j1939::is_claimable_address(config.local_address)) &&
         config.max_packets_per_cts != 0U &&
         config.timestamp_domain.valid() &&
         config.max_timestamp_uncertainty.count() >= 0;
}

bool EtpReceiver::observe_time(
    const time::MonotonicClockReading& reading) noexcept {
  if (!time::is_valid_clock_reading(
          reading, config_.timestamp_domain) ||
      reading.uncertainty >
          config_.max_timestamp_uncertainty ||
      (has_last_observed_time_ &&
       reading.value < last_observed_time_)) {
    latch_fault(true);
    return false;
  }

  last_observed_time_ = reading.value;
  has_last_observed_time_ = true;
  return true;
}

bool EtpReceiver::set_deadline_after(
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

time::MonotonicTime EtpReceiver::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

void EtpReceiver::handle_cm(
    const transport::CanFrame&,
    const EtpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  switch (cm.control) {
    case EtpControl::rts:
      start_session(cm, timestamp);
      break;
    case EtpControl::dpo:
      accept_dpo(cm, timestamp);
      break;
    case EtpControl::abort:
      if (session_.active &&
          cm.source_address == session_.source_address &&
          cm.destination_address == config_.local_address &&
          cm.transported_pgn == session_.pgn) {
        abort_session(true);
      }
      break;
    case EtpControl::cts:
    case EtpControl::eoma:
      break;
  }
}

void EtpReceiver::handle_dt(
    const EtpDtFrame& dt,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (!session_.active ||
      dt.source_address != session_.source_address ||
      dt.destination_address != config_.local_address) {
    return;
  }
  accept_dt(dt, timestamp);
}

void EtpReceiver::start_session(
    const EtpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (config_.local_address == j1939::kNullAddress ||
      cm.destination_address != config_.local_address) {
    return;
  }

  if (!j1939::is_claimable_address(cm.source_address) ||
      !valid_etp_size(cm.message_size) ||
      !valid_transported_pgn(cm.transported_pgn)) {
    saturating_increment(counters_.malformed_frames);
    return;
  }

  if (session_.active) {
    (void)queue_abort(
        cm.source_address,
        EtpAbortReason::already_in_session,
        cm.transported_pgn);
    saturating_increment(counters_.rejected_sessions);
    return;
  }

  if (sink_ == nullptr ||
      !sink_->begin(
          cm.transported_pgn,
          cm.source_address,
          config_.local_address,
          cm.message_size)) {
    saturating_increment(counters_.sink_failures);
    (void)queue_abort(
        cm.source_address,
        EtpAbortReason::resources,
        cm.transported_pgn);
    return;
  }

  session_ = {};
  session_.active = true;
  session_.phase = Phase::wait_dpo;
  session_.source_address = cm.source_address;
  session_.pgn = cm.transported_pgn;
  session_.total_size = cm.message_size;
  session_.total_packets = etp_packet_count(cm.message_size);

  if (!queue_next_cts(timestamp)) {
    abort_session(true);
    return;
  }

  saturating_increment(counters_.sessions_started);
}

void EtpReceiver::accept_dpo(
    const EtpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (!session_.active ||
      cm.source_address != session_.source_address ||
      cm.destination_address != config_.local_address ||
      cm.transported_pgn != session_.pgn) {
    return;
  }

  const auto remaining =
      session_.total_packets - session_.packets_received;
  if (session_.phase != Phase::wait_dpo ||
      cm.block_count == 0U ||
      cm.block_count > session_.granted_packets ||
      static_cast<std::uint32_t>(cm.block_count) > remaining ||
      cm.packet_offset != session_.packets_received) {
    saturating_increment(counters_.malformed_frames);
    abort_session(true);
    return;
  }

  session_.phase = Phase::receive_block;
  session_.block_offset = cm.packet_offset;
  session_.block_count = cm.block_count;
  session_.block_received = 0U;
  session_.expected_sequence = 1U;
  if (!set_deadline_after(
          timestamp, kPacketTimeout, session_.deadline)) {
    latch_fault(true);
  }
}

void EtpReceiver::accept_dt(
    const EtpDtFrame& dt,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (session_.phase != Phase::receive_block ||
      dt.sequence_number != session_.expected_sequence ||
      session_.block_received >= session_.block_count) {
    saturating_increment(counters_.malformed_frames);
    abort_session(true);
    return;
  }

  const auto absolute_packet =
      session_.block_offset +
      static_cast<std::uint32_t>(session_.block_received);
  if (absolute_packet != session_.packets_received ||
      absolute_packet >= session_.total_packets) {
    saturating_increment(counters_.malformed_frames);
    abort_session(true);
    return;
  }

  const auto byte_offset = absolute_packet * 7U;
  const auto remaining_bytes =
      session_.total_size - byte_offset;
  const auto valid_bytes = static_cast<std::uint8_t>(
      remaining_bytes < 7U ? remaining_bytes : 7U);

  if (valid_bytes < 7U) {
    for (std::size_t i = valid_bytes;
         i < dt.data.size();
         ++i) {
      if (dt.data[i] != static_cast<std::byte>(0xFFU)) {
        saturating_increment(counters_.malformed_frames);
        abort_session(true);
        return;
      }
    }
  }

  if (sink_ == nullptr ||
      !sink_->write(byte_offset, dt.data, valid_bytes)) {
    saturating_increment(counters_.sink_failures);
    (void)queue_abort(
        session_.source_address,
        EtpAbortReason::resources,
        session_.pgn);
    abort_session(true);
    return;
  }

  ++session_.packets_received;
  ++session_.block_received;
  ++session_.expected_sequence;

  if (session_.packets_received == session_.total_packets) {
    if (sink_ == nullptr || !sink_->commit()) {
      saturating_increment(counters_.sink_failures);
      (void)queue_abort(
          session_.source_address,
          EtpAbortReason::resources,
          session_.pgn);
      abort_session(true);
      return;
    }
    if (!queue_eoma()) {
      abort_session(false);
      return;
    }
    saturating_increment(counters_.completed_messages);
    abort_session(false);
    return;
  }

  if (session_.block_received == session_.block_count) {
    if (!queue_next_cts(timestamp)) {
      abort_session(true);
    }
    return;
  }

  if (!set_deadline_after(
          timestamp, kPacketTimeout, session_.deadline)) {
    latch_fault(true);
  }
}

bool EtpReceiver::queue_next_cts(
    const time::MonotonicClockReading& timestamp) noexcept {
  const auto remaining =
      session_.total_packets - session_.packets_received;
  const auto grant = static_cast<std::uint8_t>(
      remaining < config_.max_packets_per_cts
          ? remaining
          : config_.max_packets_per_cts);
  if (grant == 0U) {
    return false;
  }

  transport::CanFrame frame{};
  if (!build_etp_cts(
          config_.local_address,
          session_.source_address,
          grant,
          session_.packets_received + 1U,
          session_.pgn,
          frame) ||
      !enqueue_tx(frame)) {
    return false;
  }

  session_.granted_packets = grant;
  session_.phase = Phase::wait_dpo;
  return set_deadline_after(
      timestamp, kControlTimeout, session_.deadline);
}

bool EtpReceiver::queue_eoma() noexcept {
  transport::CanFrame frame{};
  if (!build_etp_eoma(
          config_.local_address,
          session_.source_address,
          session_.total_size,
          session_.pgn,
          frame)) {
    latch_fault(false);
    return false;
  }
  return enqueue_tx(frame);
}

bool EtpReceiver::queue_abort(
    const std::uint8_t destination,
    const EtpAbortReason reason,
    const std::uint32_t pgn) noexcept {
  if (config_.local_address == j1939::kNullAddress) {
    return false;
  }
  transport::CanFrame frame{};
  if (!build_etp_abort(
          config_.local_address,
          destination,
          reason,
          pgn,
          frame)) {
    latch_fault(true);
    return false;
  }
  return enqueue_tx(frame);
}

bool EtpReceiver::enqueue_tx(
    const transport::CanFrame& frame) noexcept {
  if (tx_count_ >= tx_queue_.size()) {
    status_ = EtpReceiverStatus::queue_overflow;
    saturating_increment(counters_.tx_queue_overflows);
    return false;
  }
  tx_queue_[tx_tail_] = frame;
  tx_tail_ = (tx_tail_ + 1U) % tx_queue_.size();
  ++tx_count_;
  status_ = EtpReceiverStatus::ok;
  return true;
}

void EtpReceiver::abort_session(
    const bool notify_sink) noexcept {
  if (notify_sink &&
      session_.active &&
      sink_ != nullptr) {
    sink_->abort();
  }
  session_ = {};
}

void EtpReceiver::latch_fault(
    const bool notify_sink) noexcept {
  if (notify_sink &&
      session_.active &&
      sink_ != nullptr) {
    sink_->abort();
  }
  session_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  status_ = EtpReceiverStatus::protocol_fault;
}

bool EtpTransmitter::configure(
    const EtpTransmitterConfig& config) noexcept {
  if (!valid_config(config) || session_.active) {
    return false;
  }

  config_ = config;
  source_ = nullptr;
  session_ = {};
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  last_observed_time_ = time::MonotonicTime{0};
  has_last_observed_time_ = false;
  configured_ = true;
  status_ = EtpTransmitterStatus::ok;
  counters_ = {};
  return true;
}

bool EtpTransmitter::set_local_address(
    const std::uint8_t address) noexcept {
  if (!configured_ ||
      session_.active ||
      (address != j1939::kNullAddress &&
       !j1939::is_claimable_address(address))) {
    return false;
  }
  config_.local_address = address;
  return true;
}

EtpTransmitterStatus EtpTransmitter::submit(
    const EtpTransmitRequest& request,
    IEtpTransmitSource& source,
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_ ||
      status_ == EtpTransmitterStatus::protocol_fault ||
      status_ == EtpTransmitterStatus::queue_overflow ||
      config_.local_address == j1939::kNullAddress) {
    return EtpTransmitterStatus::invalid_state;
  }
  if (session_.active) {
    return EtpTransmitterStatus::busy;
  }
  if (!valid_request(request)) {
    return EtpTransmitterStatus::invalid_argument;
  }
  if (!observe_time(now)) {
    return EtpTransmitterStatus::invalid_time;
  }

  session_ = {};
  session_.active = true;
  session_.phase = Phase::wait_cts;
  session_.destination_address = request.destination_address;
  session_.pgn = request.pgn;
  session_.total_size = request.total_size;
  session_.total_packets = etp_packet_count(request.total_size);
  source_ = &source;

  transport::CanFrame frame{};
  if (!build_etp_rts(
          config_.local_address,
          session_.destination_address,
          session_.total_size,
          session_.pgn,
          frame) ||
      !enqueue_tx(frame) ||
      !set_deadline_after(
          now, kResponseTimeout, session_.deadline)) {
    if (status_ == EtpTransmitterStatus::ok) {
      latch_fault();
    }
    return status_;
  }

  saturating_increment(counters_.sessions_started);
  return EtpTransmitterStatus::ok;
}

void EtpTransmitter::on_can_frame(
    const transport::ReceivedCanFrame& frame) noexcept {
  if (!configured_ ||
      !session_.active ||
      status_ == EtpTransmitterStatus::protocol_fault ||
      status_ == EtpTransmitterStatus::queue_overflow) {
    return;
  }

  EtpCmFrame cm{};
  if (!decode_etp_cm(frame.frame, cm)) {
    return;
  }
  if (cm.source_address != session_.destination_address ||
      cm.destination_address != config_.local_address ||
      cm.transported_pgn != session_.pgn) {
    return;
  }
  if (!observe_time(frame.timestamp)) {
    return;
  }

  handle_control(frame.frame, cm, frame.timestamp);
}

EtpTransmitterStatus EtpTransmitter::service_time(
    const time::MonotonicClockReading& now) noexcept {
  if (!configured_) {
    return EtpTransmitterStatus::invalid_state;
  }
  if (status_ == EtpTransmitterStatus::protocol_fault ||
      status_ == EtpTransmitterStatus::queue_overflow) {
    return status_;
  }
  if (!observe_time(now)) {
    return EtpTransmitterStatus::invalid_time;
  }

  if (!session_.active) {
    return EtpTransmitterStatus::no_action;
  }

  const auto lower = lower_bound(now);
  if (lower > session_.deadline) {
    if (!queue_abort(EtpAbortReason::timeout)) {
      reset_session();
      return status_;
    }
    saturating_increment(counters_.timeouts);
    reset_session();
    return EtpTransmitterStatus::ok;
  }

  if (session_.phase == Phase::send_block) {
    if (!queue_next_dt(now)) {
      return status_;
    }
    return EtpTransmitterStatus::ok;
  }

  return EtpTransmitterStatus::no_action;
}

bool EtpTransmitter::try_take_tx(
    transport::CanFrame& frame) noexcept {
  if (tx_count_ == 0U) {
    return false;
  }
  frame = tx_queue_[tx_head_];
  tx_head_ = (tx_head_ + 1U) % tx_queue_.size();
  --tx_count_;
  return true;
}

EtpTransmitterStatus EtpTransmitter::status() const noexcept {
  return status_;
}

EtpTransmitterCounters EtpTransmitter::counters() const noexcept {
  return counters_;
}

bool EtpTransmitter::active() const noexcept {
  return session_.active;
}

std::size_t EtpTransmitter::pending_tx_count() const noexcept {
  return tx_count_;
}

bool EtpTransmitter::valid_config(
    const EtpTransmitterConfig& config) const noexcept {
  return (config.local_address == j1939::kNullAddress ||
          j1939::is_claimable_address(config.local_address)) &&
         config.timestamp_domain.valid() &&
         config.max_timestamp_uncertainty.count() >= 0;
}

bool EtpTransmitter::valid_request(
    const EtpTransmitRequest& request) const noexcept {
  return request.destination_address != j1939::kGlobalAddress &&
         j1939::is_claimable_address(request.destination_address) &&
         valid_etp_size(request.total_size) &&
         valid_transported_pgn(request.pgn);
}

bool EtpTransmitter::observe_time(
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

bool EtpTransmitter::set_deadline_after(
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

time::MonotonicTime EtpTransmitter::lower_bound(
    const time::MonotonicClockReading& reading) const noexcept {
  if (reading.value.count() <= reading.uncertainty.count()) {
    return time::MonotonicTime{0};
  }
  return time::MonotonicTime{
      reading.value.count() - reading.uncertainty.count()};
}

void EtpTransmitter::handle_control(
    const transport::CanFrame& frame,
    const EtpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  switch (cm.control) {
    case EtpControl::cts:
      if (session_.phase != Phase::wait_cts) {
        saturating_increment(counters_.malformed_control_frames);
        return;
      }
      if (!start_block(frame, cm, timestamp)) {
        return;
      }
      break;

    case EtpControl::eoma:
      if (session_.phase != Phase::wait_eoma ||
          cm.message_size != session_.total_size) {
        saturating_increment(counters_.malformed_control_frames);
        return;
      }
      saturating_increment(counters_.completed_messages);
      reset_session();
      break;

    case EtpControl::abort:
      saturating_increment(counters_.remote_aborts);
      reset_session();
      break;

    case EtpControl::rts:
    case EtpControl::dpo:
      break;
  }
}

bool EtpTransmitter::start_block(
    const transport::CanFrame&,
    const EtpCmFrame& cm,
    const time::MonotonicClockReading& timestamp) noexcept {
  if (cm.next_packet == 0U ||
      cm.next_packet > session_.total_packets) {
    saturating_increment(counters_.malformed_control_frames);
    return false;
  }

  if (cm.packets_allowed == 0U) {
    if (!set_deadline_after(
            timestamp, kHoldTimeout, session_.deadline)) {
      latch_fault();
      return false;
    }
    return true;
  }

  const auto offset = cm.next_packet - 1U;
  const auto remaining = session_.total_packets - offset;
  const auto block_count = static_cast<std::uint8_t>(
      remaining < cm.packets_allowed
          ? remaining
          : cm.packets_allowed);
  if (block_count == 0U) {
    saturating_increment(counters_.malformed_control_frames);
    return false;
  }

  transport::CanFrame dpo{};
  if (!build_etp_dpo(
          config_.local_address,
          session_.destination_address,
          block_count,
          offset,
          session_.pgn,
          dpo) ||
      !enqueue_tx(dpo)) {
    return false;
  }

  saturating_increment(counters_.dpo_frames);
  session_.packet_offset = offset;
  session_.block_count = block_count;
  session_.next_sequence = 1U;
  session_.block_remaining = block_count;
  session_.phase = Phase::send_block;
  if (!set_deadline_after(
          timestamp, kProgressTimeout, session_.deadline)) {
    latch_fault();
    return false;
  }
  return true;
}

bool EtpTransmitter::queue_next_dt(
    const time::MonotonicClockReading& now) noexcept {
  if (!session_.active ||
      session_.phase != Phase::send_block ||
      session_.block_remaining == 0U ||
      source_ == nullptr) {
    latch_fault();
    return false;
  }

  const auto absolute_packet =
      session_.packet_offset +
      static_cast<std::uint32_t>(session_.next_sequence - 1U);
  if (absolute_packet >= session_.total_packets) {
    latch_fault();
    return false;
  }

  const auto byte_offset = absolute_packet * 7U;
  const auto remaining =
      session_.total_size - byte_offset;
  const auto expected_valid = static_cast<std::uint8_t>(
      remaining < 7U ? remaining : 7U);

  std::array<std::byte, kEtpPacketPayloadBytes> data{};
  for (auto& byte : data) {
    byte = static_cast<std::byte>(0xFFU);
  }
  std::uint8_t valid_bytes = 0U;
  if (!source_->read(byte_offset, data, valid_bytes) ||
      valid_bytes != expected_valid) {
    saturating_increment(counters_.source_failures);
    (void)queue_abort(EtpAbortReason::resources);
    reset_session();
    return false;
  }
  for (std::size_t i = valid_bytes; i < data.size(); ++i) {
    data[i] = static_cast<std::byte>(0xFFU);
  }

  transport::CanFrame frame{};
  if (!build_etp_dt(
          config_.local_address,
          session_.destination_address,
          session_.next_sequence,
          data,
          frame) ||
      !enqueue_tx(frame)) {
    return false;
  }
  saturating_increment(counters_.dt_frames);

  --session_.block_remaining;
  ++session_.next_sequence;

  const auto sent_packets =
      absolute_packet + 1U;
  if (sent_packets == session_.total_packets) {
    session_.phase = Phase::wait_eoma;
    return set_deadline_after(
        now, kResponseTimeout, session_.deadline);
  }

  if (session_.block_remaining == 0U) {
    session_.phase = Phase::wait_cts;
    return set_deadline_after(
        now, kResponseTimeout, session_.deadline);
  }

  return set_deadline_after(
      now, kProgressTimeout, session_.deadline);
}

bool EtpTransmitter::queue_abort(
    const EtpAbortReason reason) noexcept {
  if (!session_.active) {
    return false;
  }

  transport::CanFrame frame{};
  if (!build_etp_abort(
          config_.local_address,
          session_.destination_address,
          reason,
          session_.pgn,
          frame)) {
    latch_fault();
    return false;
  }
  return enqueue_tx(frame);
}

bool EtpTransmitter::enqueue_tx(
    const transport::CanFrame& frame) noexcept {
  if (tx_count_ >= tx_queue_.size()) {
    status_ = EtpTransmitterStatus::queue_overflow;
    saturating_increment(counters_.tx_queue_overflows);
    reset_session();
    return false;
  }
  tx_queue_[tx_tail_] = frame;
  tx_tail_ = (tx_tail_ + 1U) % tx_queue_.size();
  ++tx_count_;
  status_ = EtpTransmitterStatus::ok;
  return true;
}

void EtpTransmitter::reset_session() noexcept {
  session_ = {};
  source_ = nullptr;
}

void EtpTransmitter::latch_fault() noexcept {
  session_ = {};
  source_ = nullptr;
  tx_head_ = 0U;
  tx_tail_ = 0U;
  tx_count_ = 0U;
  status_ = EtpTransmitterStatus::protocol_fault;
}

}  // namespace ecu::core::v2::protocol::isobus
