#include "ecu/core_v2/protocol/j1939/transport_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::j1939;

constexpr time::MonotonicClockDomainId kDomain{0x5450U};

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

time::MonotonicClockReading reading(
    const std::int64_t ns,
    const std::int64_t uncertainty = 0) {
  return {
      time::MonotonicClockStatus::ok,
      kDomain,
      time::MonotonicTime{ns},
      time::MonotonicDuration{uncertainty}};
}

transport::ReceivedCanFrame cm_frame(
    const TpControl control,
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::uint16_t size,
    const std::uint8_t packets,
    const std::uint8_t parameter,
    const std::uint32_t pgn,
    const std::int64_t timestamp) {
  transport::ReceivedCanFrame received{};
  const std::uint8_t payload[8] = {
      static_cast<std::uint8_t>(control),
      static_cast<std::uint8_t>(size & 0xFFU),
      static_cast<std::uint8_t>((size >> 8U) & 0xFFU),
      packets,
      parameter,
      static_cast<std::uint8_t>(pgn & 0xFFU),
      static_cast<std::uint8_t>((pgn >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((pgn >> 16U) & 0xFFU)};
  const bool built = build_classic_data_frame(
      MessageAddress{
          7U,
          kTpCmPgn,
          source,
          destination},
      payload,
      8U,
      received.frame);
  if (!built) {
    return received;
  }
  received.timestamp = reading(timestamp);
  return received;
}

transport::ReceivedCanFrame dt_frame(
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::uint8_t sequence,
    const std::array<std::uint8_t, 7U>& data,
    const std::int64_t timestamp) {
  transport::ReceivedCanFrame received{};
  std::uint8_t payload[8] = {
      sequence, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  for (std::size_t i = 0U; i < data.size(); ++i) {
    payload[i + 1U] = data[i];
  }
  const bool built = build_classic_data_frame(
      MessageAddress{
          7U,
          kTpDtPgn,
          source,
          destination},
      payload,
      8U,
      received.frame);
  if (!built) {
    return received;
  }
  received.timestamp = reading(timestamp);
  return received;
}

TpReceiverConfig config(
    const std::uint8_t local = 0x80U,
    const std::uint8_t cts = 16U) {
  TpReceiverConfig value{};
  value.local_address = local;
  value.max_packets_per_cts = cts;
  value.timestamp_domain = kDomain;
  value.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  return value;
}

bool is_cm_control(
    const transport::CanFrame& frame,
    const TpControl control,
    const std::uint8_t source,
    const std::uint8_t destination) {
  IdentifierFields fields{};
  std::uint8_t actual_destination = 0U;
  return decode_classic_frame_identifier(frame, fields) &&
         parameter_group_number(fields) == kTpCmPgn &&
         destination_address(fields, actual_destination) &&
         fields.source_address == source &&
         actual_destination == destination &&
         frame.length == 8U &&
         std::to_integer<std::uint8_t>(frame.payload[0]) ==
             static_cast<std::uint8_t>(control);
}

}  // namespace

int main() {
  int failures = 0;

  {
    const auto bam = cm_frame(
        TpControl::bam,
        0x81U,
        kGlobalAddress,
        15U,
        3U,
        0xFFU,
        0xEA5FU,
        0);
    TpCmFrame decoded{};
    failures += require(
        decode_tp_cm(bam.frame, decoded) &&
            decoded.control == TpControl::bam &&
            decoded.source_address == 0x81U &&
            decoded.destination_address == kGlobalAddress &&
            decoded.message_size == 15U &&
            decoded.packet_count == 3U &&
            decoded.control_parameter == 0xFFU &&
            decoded.transported_pgn == 0xEA5FU,
        "BAM CM decode");

    const auto dt = dt_frame(
        0x81U,
        kGlobalAddress,
        1U,
        {1U, 2U, 3U, 4U, 5U, 6U, 7U},
        1);
    TpDtFrame decoded_dt{};
    failures += require(
        decode_tp_dt(dt.frame, decoded_dt) &&
            decoded_dt.sequence_number == 1U &&
            std::to_integer<std::uint8_t>(
                decoded_dt.data[6]) == 7U,
        "TP.DT decode");
  }

  {
    transport::CanFrame frame{};
    failures += require(
        build_tp_cts(
            0x80U, 0x81U, 3U, 1U, 0xEA5FU, frame) &&
            is_cm_control(
                frame, TpControl::cts, 0x80U, 0x81U) &&
            std::to_integer<std::uint8_t>(frame.payload[1]) == 3U &&
            std::to_integer<std::uint8_t>(frame.payload[2]) == 1U,
        "CTS builder");
    failures += require(
        build_tp_end_of_message_ack(
            0x80U, 0x81U, 15U, 3U, 0xEA5FU, frame) &&
            is_cm_control(
                frame,
                TpControl::end_of_message_ack,
                0x80U,
                0x81U),
        "EOM ACK builder");
    failures += require(
        build_tp_abort(
            0x80U,
            0x81U,
            TpAbortReason::timeout,
            0xEA5FU,
            frame) &&
            is_cm_control(
                frame, TpControl::abort, 0x80U, 0x81U) &&
            std::to_integer<std::uint8_t>(frame.payload[1]) ==
                static_cast<std::uint8_t>(
                    TpAbortReason::timeout),
        "Abort builder");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config()),
        "BAM receiver config");

    receiver.on_can_frame(cm_frame(
        TpControl::bam,
        0x81U,
        kGlobalAddress,
        15U,
        3U,
        0xFFU,
        0xEA5FU,
        0));
    failures += require(
        receiver.bam_active(),
        "BAM session starts");

    receiver.on_can_frame(dt_frame(
        0x81U,
        kGlobalAddress,
        1U,
        {1U, 2U, 3U, 4U, 5U, 6U, 7U},
        50000000));
    receiver.on_can_frame(dt_frame(
        0x81U,
        kGlobalAddress,
        2U,
        {8U, 9U, 10U, 11U, 12U, 13U, 14U},
        100000000));
    receiver.on_can_frame(dt_frame(
        0x81U,
        kGlobalAddress,
        3U,
        {15U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU},
        150000000));

    TpMessage message{};
    failures += require(
        !receiver.bam_active() &&
            receiver.try_take_message(message) &&
            message.broadcast &&
            message.source_address == 0x81U &&
            message.destination_address == kGlobalAddress &&
            message.pgn == 0xEA5FU &&
            message.size == 15U &&
            std::to_integer<std::uint8_t>(message.data[0]) == 1U &&
            std::to_integer<std::uint8_t>(message.data[14]) == 15U &&
            receiver.pending_tx_count() == 0U,
        "BAM reassembly completes without flow-control TX");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config()),
        "BAM padding config");
    receiver.on_can_frame(cm_frame(
        TpControl::bam,
        0x81U,
        kGlobalAddress,
        9U,
        2U,
        0xFFU,
        0xF100U,
        0));
    receiver.on_can_frame(dt_frame(
        0x81U,
        kGlobalAddress,
        1U,
        {1U, 2U, 3U, 4U, 5U, 6U, 7U},
        50000000));
    receiver.on_can_frame(dt_frame(
        0x81U,
        kGlobalAddress,
        2U,
        {8U, 9U, 0U, 0xFFU, 0xFFU, 0xFFU, 0xFFU},
        100000000));
    failures += require(
        !receiver.bam_active() &&
            receiver.pending_message_count() == 0U &&
            receiver.counters().malformed_frames == 1U,
        "last TP packet requires FF padding");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config()),
        "BAM timeout config");
    receiver.on_can_frame(cm_frame(
        TpControl::bam,
        0x81U,
        kGlobalAddress,
        9U,
        2U,
        0xFFU,
        0xF100U,
        0));
    failures += require(
        receiver.service_time(reading(750000000)) ==
                TpReceiverStatus::no_action &&
            receiver.bam_active(),
        "BAM T1 equality is not timeout");
    failures += require(
        receiver.service_time(reading(750000001)) ==
                TpReceiverStatus::ok &&
            !receiver.bam_active() &&
            receiver.counters().timeouts == 1U,
        "BAM T1 timeout closes session");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config(0x80U, 2U)),
        "RTS receiver config");
    receiver.on_can_frame(cm_frame(
        TpControl::rts,
        0x81U,
        0x80U,
        15U,
        3U,
        0xFFU,
        0xEA5FU,
        0));
    transport::CanFrame tx{};
    failures += require(
        receiver.peer_active() &&
            receiver.try_take_tx(tx) &&
            is_cm_control(
                tx, TpControl::cts, 0x80U, 0x81U) &&
            std::to_integer<std::uint8_t>(tx.payload[1]) == 2U &&
            std::to_integer<std::uint8_t>(tx.payload[2]) == 1U,
        "RTS starts peer session and grants first CTS block");

    receiver.on_can_frame(dt_frame(
        0x81U,
        0x80U,
        1U,
        {1U, 2U, 3U, 4U, 5U, 6U, 7U},
        1000000));
    receiver.on_can_frame(dt_frame(
        0x81U,
        0x80U,
        2U,
        {8U, 9U, 10U, 11U, 12U, 13U, 14U},
        2000000));
    failures += require(
        receiver.try_take_tx(tx) &&
            is_cm_control(
                tx, TpControl::cts, 0x80U, 0x81U) &&
            std::to_integer<std::uint8_t>(tx.payload[1]) == 1U &&
            std::to_integer<std::uint8_t>(tx.payload[2]) == 3U,
        "receiver issues next CTS after grant exhausted");

    receiver.on_can_frame(dt_frame(
        0x81U,
        0x80U,
        3U,
        {15U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU},
        3000000));
    TpMessage message{};
    failures += require(
        receiver.try_take_tx(tx) &&
            is_cm_control(
                tx,
                TpControl::end_of_message_ack,
                0x80U,
                0x81U) &&
            receiver.try_take_message(message) &&
            !message.broadcast &&
            message.size == 15U &&
            message.pgn == 0xEA5FU &&
            !receiver.peer_active(),
        "RTS/CTS completes with EOM ACK");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config()),
        "second RTS config");
    receiver.on_can_frame(cm_frame(
        TpControl::rts,
        0x81U,
        0x80U,
        15U,
        3U,
        0xFFU,
        0xEA5FU,
        0));
    transport::CanFrame tx{};
    failures += require(
        receiver.try_take_tx(tx),
        "drain first CTS");
    receiver.on_can_frame(cm_frame(
        TpControl::rts,
        0x82U,
        0x80U,
        9U,
        2U,
        0xFFU,
        0xF100U,
        1000));
    failures += require(
        receiver.try_take_tx(tx) &&
            is_cm_control(
                tx, TpControl::abort, 0x80U, 0x82U) &&
            std::to_integer<std::uint8_t>(tx.payload[1]) ==
                static_cast<std::uint8_t>(
                    TpAbortReason::already_in_session) &&
            receiver.counters().rejected_sessions == 1U,
        "second peer session rejected with busy Abort");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config()),
        "peer timeout config");
    receiver.on_can_frame(cm_frame(
        TpControl::rts,
        0x81U,
        0x80U,
        9U,
        2U,
        0xFFU,
        0xF100U,
        0));
    transport::CanFrame tx{};
    failures += require(
        receiver.try_take_tx(tx),
        "drain timeout CTS");
    failures += require(
        receiver.service_time(reading(1250000000)) ==
                TpReceiverStatus::no_action &&
            receiver.peer_active(),
        "CTS T2 equality not timed out");
    failures += require(
        receiver.service_time(reading(1250000001)) ==
                TpReceiverStatus::ok &&
            !receiver.peer_active() &&
            receiver.try_take_tx(tx) &&
            is_cm_control(
                tx, TpControl::abort, 0x80U, 0x81U) &&
            std::to_integer<std::uint8_t>(tx.payload[1]) ==
                static_cast<std::uint8_t>(
                    TpAbortReason::timeout),
        "peer timeout emits Abort reason timeout");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config(0x80U, 2U)),
        "parallel BAM/peer config");
    receiver.on_can_frame(cm_frame(
        TpControl::bam,
        0x90U,
        kGlobalAddress,
        9U,
        2U,
        0xFFU,
        0xF100U,
        0));
    receiver.on_can_frame(cm_frame(
        TpControl::rts,
        0x81U,
        0x80U,
        9U,
        2U,
        0xFFU,
        0xF200U,
        1));
    transport::CanFrame tx{};
    failures += require(
        receiver.bam_active() &&
            receiver.peer_active() &&
            receiver.try_take_tx(tx),
        "one BAM and one peer session coexist");

    receiver.on_can_frame(dt_frame(
        0x90U,
        kGlobalAddress,
        1U,
        {1U, 2U, 3U, 4U, 5U, 6U, 7U},
        1000000));
    receiver.on_can_frame(dt_frame(
        0x81U,
        0x80U,
        1U,
        {11U, 12U, 13U, 14U, 15U, 16U, 17U},
        1000001));
    receiver.on_can_frame(dt_frame(
        0x90U,
        kGlobalAddress,
        2U,
        {8U, 9U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU},
        2000000));
    receiver.on_can_frame(dt_frame(
        0x81U,
        0x80U,
        2U,
        {18U, 19U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU},
        2000001));
    failures += require(
        receiver.pending_message_count() == 2U &&
            !receiver.bam_active() &&
            !receiver.peer_active() &&
            receiver.try_take_tx(tx) &&
            is_cm_control(
                tx,
                TpControl::end_of_message_ack,
                0x80U,
                0x81U),
        "parallel BAM and peer sessions both complete");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config()),
        "resource reservation config");

    std::int64_t session_time = 0;
    for (const std::uint8_t source :
         std::array<std::uint8_t, 2U>{0x90U, 0x91U}) {
      receiver.on_can_frame(cm_frame(
          TpControl::bam,
          source,
          kGlobalAddress,
          9U,
          2U,
          0xFFU,
          static_cast<std::uint32_t>(0xF100U + source),
          session_time));
      receiver.on_can_frame(dt_frame(
          source,
          kGlobalAddress,
          1U,
          {1U, 2U, 3U, 4U, 5U, 6U, 7U},
          session_time + 1000));
      receiver.on_can_frame(dt_frame(
          source,
          kGlobalAddress,
          2U,
          {8U, 9U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU},
          session_time + 2000));
      session_time += 10000;
    }
    failures += require(
        receiver.pending_message_count() == 2U,
        "completed queue reserves both bounded slots");

    receiver.on_can_frame(cm_frame(
        TpControl::rts,
        0x81U,
        0x80U,
        9U,
        2U,
        0xFFU,
        0xF300U,
        session_time));
    transport::CanFrame tx{};
    failures += require(
        receiver.try_take_tx(tx) &&
            is_cm_control(
                tx, TpControl::abort, 0x80U, 0x81U) &&
            std::to_integer<std::uint8_t>(tx.payload[1]) ==
                static_cast<std::uint8_t>(
                    TpAbortReason::resources),
        "RTS rejected with resources Abort when message queue full");
  }

  {
    TpReceiver receiver;
    failures += require(
        receiver.configure(config()),
        "time fault config");
    receiver.on_can_frame(cm_frame(
        TpControl::bam,
        0x81U,
        kGlobalAddress,
        9U,
        2U,
        0xFFU,
        0xF100U,
        100));
    auto bad_time = reading(99);
    failures += require(
        receiver.service_time(bad_time) ==
                TpReceiverStatus::invalid_time &&
            receiver.status() ==
                TpReceiverStatus::protocol_fault &&
            !receiver.bam_active(),
        "backward TP time fails closed");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_J1939_TP_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
