#include "ecu/core_v2/protocol/j1939/transport_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::j1939;

constexpr time::MonotonicClockDomainId kDomain{0x5458U};

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

TpTransmitterConfig config(
    const std::uint8_t local = 0x80U,
    const std::int64_t bam_interval = 50000000) {
  TpTransmitterConfig value{};
  value.local_address = local;
  value.timestamp_domain = kDomain;
  value.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  value.bam_packet_interval =
      time::MonotonicDuration{bam_interval};
  return value;
}

TpTransmitMessage message(
    const std::uint16_t size,
    const std::uint8_t destination,
    const std::uint32_t pgn = 0xF100U) {
  TpTransmitMessage value{};
  value.pgn = pgn;
  value.destination_address = destination;
  value.size = size;
  for (std::uint16_t i = 0U; i < size; ++i) {
    value.data[i] = static_cast<std::byte>(
        static_cast<std::uint8_t>((i + 1U) & 0xFFU));
  }
  return value;
}

transport::ReceivedCanFrame wrap(
    const transport::CanFrame& frame,
    const std::int64_t timestamp) {
  transport::ReceivedCanFrame received{};
  received.frame = frame;
  received.timestamp = reading(timestamp);
  return received;
}

bool cm_control(
    const transport::CanFrame& frame,
    const TpControl control,
    const std::uint8_t source,
    const std::uint8_t destination) {
  TpCmFrame cm{};
  return decode_tp_cm(frame, cm) &&
         cm.control == control &&
         cm.source_address == source &&
         cm.destination_address == destination;
}

bool dt_sequence(
    const transport::CanFrame& frame,
    const std::uint8_t sequence,
    const std::uint8_t source,
    const std::uint8_t destination) {
  TpDtFrame dt{};
  return decode_tp_dt(frame, dt) &&
         dt.sequence_number == sequence &&
         dt.source_address == source &&
         dt.destination_address == destination;
}

transport::ReceivedCanFrame cts(
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::uint8_t allowed,
    const std::uint8_t next_packet,
    const std::uint32_t pgn,
    const std::int64_t timestamp) {
  transport::CanFrame frame{};
  (void)build_tp_cts(
      source,
      destination,
      allowed,
      next_packet,
      pgn,
      frame);
  return wrap(frame, timestamp);
}

transport::ReceivedCanFrame eom(
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::uint16_t size,
    const std::uint8_t packets,
    const std::uint32_t pgn,
    const std::int64_t timestamp) {
  transport::CanFrame frame{};
  (void)build_tp_end_of_message_ack(
      source,
      destination,
      size,
      packets,
      pgn,
      frame);
  return wrap(frame, timestamp);
}

transport::ReceivedCanFrame remote_abort(
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::uint32_t pgn,
    const std::int64_t timestamp) {
  transport::CanFrame frame{};
  (void)build_tp_abort(
      source,
      destination,
      TpAbortReason::resources,
      pgn,
      frame);
  return wrap(frame, timestamp);
}

}  // namespace

int main() {
  int failures = 0;

  {
    transport::CanFrame frame{};
    failures += require(
        build_tp_bam(
            0x80U, 15U, 3U, 0xF100U, frame) &&
            cm_control(
                frame,
                TpControl::bam,
                0x80U,
                kGlobalAddress),
        "BAM builder");
    failures += require(
        build_tp_rts(
            0x80U, 0x81U, 15U, 3U, 0xF100U, frame) &&
            cm_control(
                frame,
                TpControl::rts,
                0x80U,
                0x81U),
        "RTS builder");

    std::array<std::byte, 7U> data{};
    data[0] = static_cast<std::byte>(0x12U);
    failures += require(
        build_tp_dt(
            0x80U,
            kGlobalAddress,
            1U,
            data,
            frame) &&
            dt_sequence(
                frame,
                1U,
                0x80U,
                kGlobalAddress),
        "TP.DT builder");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "BAM transmitter config");

    auto msg = message(15U, kGlobalAddress);
    failures += require(
        transmitter.submit(msg, reading(0)) ==
                TpTransmitterStatus::ok &&
            transmitter.bam_active(),
        "BAM submit");

    transport::CanFrame tx{};
    failures += require(
        transmitter.try_take_tx(tx) &&
            cm_control(
                tx,
                TpControl::bam,
                0x80U,
                kGlobalAddress),
        "BAM CM queued first");

    failures += require(
        transmitter.service_time(reading(49999999)) ==
                TpTransmitterStatus::no_action &&
            transmitter.pending_tx_count() == 0U,
        "BAM first packet not sent before 50 ms");

    failures += require(
        transmitter.service_time(reading(50000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(
                tx, 1U, 0x80U, kGlobalAddress),
        "BAM first DT at 50 ms");

    failures += require(
        transmitter.service_time(reading(100000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(
                tx, 2U, 0x80U, kGlobalAddress),
        "BAM second DT paced");

    failures += require(
        transmitter.service_time(reading(150000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(
                tx, 3U, 0x80U, kGlobalAddress) &&
            !transmitter.bam_active() &&
            transmitter.counters().completed_messages == 1U,
        "BAM completes on last DT");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config(0x80U, 100000000)),
        "custom BAM pacing config");
    auto msg = message(9U, kGlobalAddress);
    failures += require(
        transmitter.submit(msg, reading(0)) ==
            TpTransmitterStatus::ok,
        "custom BAM submit");
    transport::CanFrame tx{};
    failures += require(
        transmitter.try_take_tx(tx),
        "drain custom BAM CM");
    failures += require(
        transmitter.service_time(reading(99999999)) ==
                TpTransmitterStatus::no_action &&
            transmitter.service_time(reading(100000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(
                tx, 1U, 0x80U, kGlobalAddress),
        "configured BAM interval applied");

    failures += require(
        !TpTransmitter{}.configure(config(0x80U, 49999999)) &&
            !TpTransmitter{}.configure(config(0x80U, 200000001)),
        "BAM pacing outside 50..200 ms rejected");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "peer transmitter config");
    auto msg = message(15U, 0x81U, 0xF200U);
    failures += require(
        transmitter.submit(msg, reading(0)) ==
                TpTransmitterStatus::ok &&
            transmitter.peer_active(),
        "peer RTS submit");

    transport::CanFrame tx{};
    failures += require(
        transmitter.try_take_tx(tx) &&
            cm_control(
                tx, TpControl::rts, 0x80U, 0x81U),
        "RTS queued first");

    transmitter.on_can_frame(
        cts(0x81U, 0x80U, 2U, 1U, 0xF200U, 100000000));
    failures += require(
        transmitter.service_time(reading(100000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(tx, 1U, 0x80U, 0x81U),
        "CTS opens first peer packet");
    failures += require(
        transmitter.service_time(reading(100000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(tx, 2U, 0x80U, 0x81U),
        "CTS block sends second peer packet");

    failures += require(
        transmitter.service_time(reading(100000001)) ==
            TpTransmitterStatus::no_action,
        "peer waits for next CTS after granted block");

    transmitter.on_can_frame(
        cts(0x81U, 0x80U, 1U, 3U, 0xF200U, 110000000));
    failures += require(
        transmitter.service_time(reading(110000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(tx, 3U, 0x80U, 0x81U) &&
            transmitter.peer_active(),
        "second CTS sends final packet and waits for EOM");

    transmitter.on_can_frame(
        eom(
            0x81U,
            0x80U,
            15U,
            3U,
            0xF200U,
            120000000));
    failures += require(
        !transmitter.peer_active() &&
            transmitter.counters().completed_messages == 1U,
        "EOM ACK completes peer transfer");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "CTS hold config");
    auto msg = message(9U, 0x81U, 0xF200U);
    failures += require(
        transmitter.submit(msg, reading(0)) ==
            TpTransmitterStatus::ok,
        "CTS hold submit");
    transport::CanFrame tx{};
    failures += require(
        transmitter.try_take_tx(tx),
        "drain CTS hold RTS");

    transmitter.on_can_frame(
        cts(0x81U, 0x80U, 0U, 1U, 0xF200U, 100000000));
    failures += require(
        transmitter.service_time(reading(1150000000)) ==
                TpTransmitterStatus::no_action &&
            transmitter.peer_active(),
        "CTS=0 hold survives through T4 equality");

    transmitter.on_can_frame(
        cts(0x81U, 0x80U, 2U, 1U, 0xF200U, 1150000000));
    failures += require(
        transmitter.service_time(reading(1150000000)) ==
                TpTransmitterStatus::ok &&
            transmitter.try_take_tx(tx) &&
            dt_sequence(tx, 1U, 0x80U, 0x81U),
        "nonzero CTS resumes held transfer");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "RTS timeout config");
    auto msg = message(9U, 0x81U, 0xF200U);
    failures += require(
        transmitter.submit(msg, reading(0)) ==
            TpTransmitterStatus::ok,
        "RTS timeout submit");
    transport::CanFrame tx{};
    failures += require(
        transmitter.try_take_tx(tx),
        "drain timeout RTS");
    failures += require(
        transmitter.service_time(reading(200000000)) ==
                TpTransmitterStatus::no_action &&
            transmitter.peer_active(),
        "RTS response timeout equality allowed");
    failures += require(
        transmitter.service_time(reading(200000001)) ==
                TpTransmitterStatus::ok &&
            !transmitter.peer_active() &&
            transmitter.try_take_tx(tx) &&
            cm_control(
                tx, TpControl::abort, 0x80U, 0x81U) &&
            transmitter.counters().timeouts == 1U,
        "RTS timeout queues Abort");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "remote Abort config");
    auto msg = message(9U, 0x81U, 0xF200U);
    failures += require(
        transmitter.submit(msg, reading(0)) ==
            TpTransmitterStatus::ok,
        "remote Abort submit");
    transport::CanFrame tx{};
    failures += require(
        transmitter.try_take_tx(tx),
        "drain remote Abort RTS");

    transmitter.on_can_frame(
        remote_abort(
            0x81U,
            0x80U,
            0xF200U,
            100000000));
    failures += require(
        !transmitter.peer_active() &&
            transmitter.counters().aborted_messages == 1U,
        "remote Abort terminates peer transfer");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "parallel TX config");
    failures += require(
        transmitter.submit(
            message(9U, kGlobalAddress, 0xF100U),
            reading(0)) ==
                TpTransmitterStatus::ok &&
            transmitter.submit(
                message(9U, 0x81U, 0xF200U),
                reading(1)) ==
                TpTransmitterStatus::ok &&
            transmitter.bam_active() &&
            transmitter.peer_active() &&
            transmitter.pending_tx_count() == 2U,
        "one BAM and one peer TX session coexist");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "invalid TX inputs config");
    auto short_msg = message(8U, kGlobalAddress);
    failures += require(
        transmitter.submit(short_msg, reading(0)) ==
            TpTransmitterStatus::invalid_argument,
        "single-packet payload rejected from TP");
    auto max_msg = message(
        static_cast<std::uint16_t>(kTpMaxMessageBytes),
        kGlobalAddress);
    failures += require(
        transmitter.submit(max_msg, reading(1)) ==
            TpTransmitterStatus::ok,
        "1785-byte BAM accepted");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "backward transmitter time config");
    failures += require(
        transmitter.submit(
            message(9U, kGlobalAddress),
            reading(100)) ==
            TpTransmitterStatus::ok,
        "backward time setup");
    failures += require(
        transmitter.service_time(reading(99)) ==
                TpTransmitterStatus::invalid_time &&
            transmitter.status() ==
                TpTransmitterStatus::protocol_fault &&
            !transmitter.bam_active(),
        "backward transmitter time fails closed");
  }

  {
    TpTransmitter transmitter;
    failures += require(
        transmitter.configure(config()),
        "TX queue overflow config");
    failures += require(
        transmitter.submit(
            message(106U, kGlobalAddress),
            reading(0)) ==
            TpTransmitterStatus::ok,
        "queue overflow BAM submit");

    std::int64_t timestamp = 50000000;
    while (transmitter.status() ==
               TpTransmitterStatus::ok &&
           transmitter.bam_active()) {
      (void)transmitter.service_time(reading(timestamp));
      timestamp += 50000000;
    }

    failures += require(
        transmitter.status() ==
                TpTransmitterStatus::queue_overflow &&
            transmitter.counters().tx_queue_overflows == 1U,
        "undrained deferred TX queue fails closed");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_J1939_TP_TX_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
