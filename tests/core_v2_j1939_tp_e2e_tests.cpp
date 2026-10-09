#include "ecu/core_v2/protocol/j1939/transport_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::j1939;

constexpr time::MonotonicClockDomainId kDomain{0xE2E0U};

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

time::MonotonicClockReading reading(const std::int64_t ns) {
  return {
      time::MonotonicClockStatus::ok,
      kDomain,
      time::MonotonicTime{ns},
      time::MonotonicDuration{0}};
}

transport::ReceivedCanFrame received(
    const transport::CanFrame& frame,
    const std::int64_t ns) {
  return {frame, reading(ns)};
}

TpTransmitMessage message(
    const std::uint16_t size,
    const std::uint8_t destination,
    const std::uint32_t pgn) {
  TpTransmitMessage value{};
  value.size = size;
  value.destination_address = destination;
  value.pgn = pgn;
  for (std::uint16_t i = 0U; i < size; ++i) {
    value.data[i] = static_cast<std::byte>(
        static_cast<std::uint8_t>((0x30U + i) & 0xFFU));
  }
  return value;
}

bool same_message(
    const TpMessage& received_message,
    const TpTransmitMessage& sent,
    const std::uint8_t source,
    const bool broadcast) {
  if (received_message.pgn != sent.pgn ||
      received_message.source_address != source ||
      received_message.destination_address !=
          sent.destination_address ||
      received_message.size != sent.size ||
      received_message.broadcast != broadcast) {
    return false;
  }
  for (std::uint16_t i = 0U; i < sent.size; ++i) {
    if (received_message.data[i] != sent.data[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  int failures = 0;

  {
    TpTransmitter tx;
    TpReceiver rx;

    TpTransmitterConfig tx_config{};
    tx_config.local_address = 0x80U;
    tx_config.timestamp_domain = kDomain;
    tx_config.max_timestamp_uncertainty =
        time::MonotonicDuration{1};
    tx_config.bam_packet_interval =
        time::MonotonicDuration{50000000};

    TpReceiverConfig rx_config{};
    rx_config.local_address = 0x81U;
    rx_config.max_packets_per_cts = 2U;
    rx_config.timestamp_domain = kDomain;
    rx_config.max_timestamp_uncertainty =
        time::MonotonicDuration{1};

    const auto sent = message(
        20U, kGlobalAddress, 0xF100U);
    failures += require(
        tx.configure(tx_config) &&
            rx.configure(rx_config) &&
            tx.submit(sent, reading(0)) ==
                TpTransmitterStatus::ok,
        "BAM end-to-end setup");

    transport::CanFrame frame{};
    failures += require(
        tx.try_take_tx(frame),
        "BAM control frame available");
    rx.on_can_frame(received(frame, 0));

    for (std::int64_t now = 50000000;
         now <= 150000000;
         now += 50000000) {
      failures += require(
          tx.service_time(reading(now)) ==
              TpTransmitterStatus::ok,
          "BAM transmitter advances");
      failures += require(
          tx.try_take_tx(frame),
          "BAM data frame available");
      rx.on_can_frame(received(frame, now));
    }

    TpMessage assembled{};
    failures += require(
        rx.try_take_message(assembled) &&
            same_message(assembled, sent, 0x80U, true) &&
            !tx.bam_active() &&
            !rx.bam_active(),
        "BAM transmitter and receiver interoperate");
  }

  {
    TpTransmitter tx;
    TpReceiver rx;

    TpTransmitterConfig tx_config{};
    tx_config.local_address = 0x80U;
    tx_config.timestamp_domain = kDomain;
    tx_config.max_timestamp_uncertainty =
        time::MonotonicDuration{1};
    tx_config.bam_packet_interval =
        time::MonotonicDuration{50000000};

    TpReceiverConfig rx_config{};
    rx_config.local_address = 0x81U;
    rx_config.max_packets_per_cts = 2U;
    rx_config.timestamp_domain = kDomain;
    rx_config.max_timestamp_uncertainty =
        time::MonotonicDuration{1};

    const auto sent = message(20U, 0x81U, 0xF200U);
    failures += require(
        tx.configure(tx_config) &&
            rx.configure(rx_config) &&
            tx.submit(sent, reading(0)) ==
                TpTransmitterStatus::ok,
        "RTS/CTS end-to-end setup");

    transport::CanFrame frame{};
    failures += require(
        tx.try_take_tx(frame),
        "RTS available");
    rx.on_can_frame(received(frame, 0));

    failures += require(
        rx.try_take_tx(frame),
        "first CTS available");
    tx.on_can_frame(received(frame, 1000000));

    failures += require(
        tx.service_time(reading(1000000)) ==
                TpTransmitterStatus::ok &&
            tx.try_take_tx(frame),
        "first DT available");
    rx.on_can_frame(received(frame, 2000000));

    failures += require(
        tx.service_time(reading(2000000)) ==
                TpTransmitterStatus::ok &&
            tx.try_take_tx(frame),
        "second DT available");
    rx.on_can_frame(received(frame, 3000000));

    failures += require(
        rx.try_take_tx(frame),
        "second CTS available");
    tx.on_can_frame(received(frame, 4000000));

    failures += require(
        tx.service_time(reading(4000000)) ==
                TpTransmitterStatus::ok &&
            tx.try_take_tx(frame),
        "final DT available");
    rx.on_can_frame(received(frame, 5000000));

    failures += require(
        rx.try_take_tx(frame),
        "EOM ACK available");
    tx.on_can_frame(received(frame, 6000000));

    TpMessage assembled{};
    failures += require(
        rx.try_take_message(assembled) &&
            same_message(assembled, sent, 0x80U, false) &&
            !tx.peer_active() &&
            !rx.peer_active() &&
            tx.counters().completed_messages == 1U &&
            rx.counters().completed_messages == 1U,
        "RTS/CTS transmitter and receiver interoperate");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_J1939_TP_E2E_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
