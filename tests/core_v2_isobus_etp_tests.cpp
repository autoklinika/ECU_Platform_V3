#include "ecu/core_v2/protocol/isobus/extended_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::isobus;
namespace j1939 = ecu::core::v2::protocol::j1939;

constexpr time::MonotonicClockDomainId kDomain{0x117836U};
constexpr std::uint8_t kTxAddress = 0x80U;
constexpr std::uint8_t kRxAddress = 0x81U;
constexpr std::uint32_t kPayloadPgn = 0xF100U;

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

transport::ReceivedCanFrame wrap(
    const transport::CanFrame& frame,
    const std::int64_t ns) {
  return {frame, reading(ns)};
}

EtpReceiverConfig receiver_config(
    const std::uint8_t grant = 255U) {
  EtpReceiverConfig config{};
  config.local_address = kRxAddress;
  config.max_packets_per_cts = grant;
  config.timestamp_domain = kDomain;
  config.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  return config;
}

EtpTransmitterConfig transmitter_config() {
  EtpTransmitterConfig config{};
  config.local_address = kTxAddress;
  config.timestamp_domain = kDomain;
  config.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  return config;
}

class PatternSource final : public IEtpTransmitSource {
 public:
  explicit PatternSource(const std::uint32_t size) noexcept
      : size_(size) {}

  [[nodiscard]] bool read(
      const std::uint32_t byte_offset,
      std::array<std::byte, kEtpPacketPayloadBytes>& data,
      std::uint8_t& valid_bytes) noexcept override {
    ++reads;
    if (fail && byte_offset >= fail_offset) {
      return false;
    }
    if (byte_offset >= size_) {
      return false;
    }

    const auto remaining = size_ - byte_offset;
    valid_bytes = static_cast<std::uint8_t>(
        remaining < 7U ? remaining : 7U);
    for (std::uint8_t i = 0U; i < valid_bytes; ++i) {
      data[i] = static_cast<std::byte>(
          static_cast<std::uint8_t>(
              (byte_offset + i + 0x31U) & 0xFFU));
    }
    return true;
  }

  bool fail{false};
  std::uint32_t fail_offset{0U};
  std::uint32_t reads{0U};

 private:
  std::uint32_t size_{0U};
};

class BufferSink final : public IEtpReceiveSink {
 public:
  static constexpr std::size_t kCapacity = 4096U;

  [[nodiscard]] bool begin(
      const std::uint32_t pgn,
      const std::uint8_t source_address,
      const std::uint8_t destination_address,
      const std::uint32_t total_size) noexcept override {
    ++begins;
    if (fail_begin || total_size > kCapacity) {
      return false;
    }
    pgn_value = pgn;
    source = source_address;
    destination = destination_address;
    size = total_size;
    written = 0U;
    active = true;
    committed = false;
    return true;
  }

  [[nodiscard]] bool write(
      const std::uint32_t byte_offset,
      const std::array<std::byte, kEtpPacketPayloadBytes>& data,
      const std::uint8_t valid_bytes) noexcept override {
    ++writes;
    if (!active ||
        fail_write ||
        valid_bytes == 0U ||
        valid_bytes > data.size() ||
        byte_offset + valid_bytes > size ||
        byte_offset + valid_bytes > buffer.size()) {
      return false;
    }
    for (std::uint8_t i = 0U; i < valid_bytes; ++i) {
      buffer[byte_offset + i] = data[i];
    }
    written += valid_bytes;
    return true;
  }

  [[nodiscard]] bool commit() noexcept override {
    ++commits;
    if (!active || fail_commit || written != size) {
      return false;
    }
    active = false;
    committed = true;
    return true;
  }

  void abort() noexcept override {
    ++aborts;
    active = false;
  }

  [[nodiscard]] bool pattern_matches() const noexcept {
    if (!committed) {
      return false;
    }
    for (std::uint32_t i = 0U; i < size; ++i) {
      const auto expected = static_cast<std::byte>(
          static_cast<std::uint8_t>(
              (i + 0x31U) & 0xFFU));
      if (buffer[i] != expected) {
        return false;
      }
    }
    return true;
  }

  std::array<std::byte, kCapacity> buffer{};
  std::uint32_t pgn_value{0U};
  std::uint32_t size{0U};
  std::uint32_t written{0U};
  std::uint32_t begins{0U};
  std::uint32_t writes{0U};
  std::uint32_t commits{0U};
  std::uint32_t aborts{0U};
  std::uint8_t source{0U};
  std::uint8_t destination{0U};
  bool active{false};
  bool committed{false};
  bool fail_begin{false};
  bool fail_write{false};
  bool fail_commit{false};
};

class CountingSink final : public IEtpReceiveSink {
 public:
  [[nodiscard]] bool begin(
      const std::uint32_t pgn,
      const std::uint8_t source_address,
      const std::uint8_t destination_address,
      const std::uint32_t total_size) noexcept override {
    ++begins;
    pgn_value = pgn;
    source = source_address;
    destination = destination_address;
    size = total_size;
    active = true;
    return !fail_begin;
  }

  [[nodiscard]] bool write(
      const std::uint32_t,
      const std::array<std::byte, kEtpPacketPayloadBytes>&,
      const std::uint8_t) noexcept override {
    ++writes;
    return true;
  }

  [[nodiscard]] bool commit() noexcept override {
    ++commits;
    active = false;
    return true;
  }

  void abort() noexcept override {
    ++aborts;
    active = false;
  }

  std::uint32_t pgn_value{0U};
  std::uint32_t size{0U};
  std::uint32_t begins{0U};
  std::uint32_t writes{0U};
  std::uint32_t commits{0U};
  std::uint32_t aborts{0U};
  std::uint8_t source{0U};
  std::uint8_t destination{0U};
  bool active{false};
  bool fail_begin{false};
};

bool is_control(
    const transport::CanFrame& frame,
    const EtpControl control,
    const std::uint8_t source,
    const std::uint8_t destination) {
  EtpCmFrame cm{};
  return decode_etp_cm(frame, cm) &&
         cm.control == control &&
         cm.source_address == source &&
         cm.destination_address == destination;
}

transport::ReceivedCanFrame remote_abort(
    const std::uint32_t pgn,
    const std::int64_t ns) {
  transport::CanFrame frame{};
  (void)build_etp_abort(
      kRxAddress,
      kTxAddress,
      EtpAbortReason::resources,
      pgn,
      frame);
  return wrap(frame, ns);
}

}  // namespace

int main() {
  int failures = 0;

  failures += require(
      sizeof(EtpReceiver) < 4096U &&
          sizeof(EtpTransmitter) < 4096U,
      "ETP state machines remain streaming-sized");

  {
    transport::CanFrame frame{};
    failures += require(
        build_etp_rts(
            kTxAddress,
            kRxAddress,
            0x00123456U,
            kPayloadPgn,
            frame),
        "ETP RTS builder");
    EtpCmFrame cm{};
    failures += require(
        decode_etp_cm(frame, cm) &&
            cm.control == EtpControl::rts &&
            cm.source_address == kTxAddress &&
            cm.destination_address == kRxAddress &&
            cm.message_size == 0x00123456U &&
            cm.transported_pgn == kPayloadPgn &&
            std::to_integer<std::uint8_t>(frame.payload[1]) == 0x56U &&
            std::to_integer<std::uint8_t>(frame.payload[2]) == 0x34U &&
            std::to_integer<std::uint8_t>(frame.payload[3]) == 0x12U &&
            std::to_integer<std::uint8_t>(frame.payload[4]) == 0x00U,
        "ETP RTS exact 32-bit size fields");

    failures += require(
        build_etp_cts(
            kRxAddress,
            kTxAddress,
            0x10U,
            0x012345U,
            kPayloadPgn,
            frame) &&
            decode_etp_cm(frame, cm) &&
            cm.control == EtpControl::cts &&
            cm.packets_allowed == 0x10U &&
            cm.next_packet == 0x012345U &&
            std::to_integer<std::uint8_t>(frame.payload[2]) == 0x45U &&
            std::to_integer<std::uint8_t>(frame.payload[3]) == 0x23U &&
            std::to_integer<std::uint8_t>(frame.payload[4]) == 0x01U,
        "ETP CTS exact 24-bit next-packet fields");

    failures += require(
        build_etp_dpo(
            kTxAddress,
            kRxAddress,
            0x20U,
            0x012345U,
            kPayloadPgn,
            frame) &&
            decode_etp_cm(frame, cm) &&
            cm.control == EtpControl::dpo &&
            cm.block_count == 0x20U &&
            cm.packet_offset == 0x012345U,
        "ETP DPO exact 24-bit offset fields");

    failures += require(
        build_etp_eoma(
            kRxAddress,
            kTxAddress,
            0x00123456U,
            kPayloadPgn,
            frame) &&
            decode_etp_cm(frame, cm) &&
            cm.control == EtpControl::eoma &&
            cm.message_size == 0x00123456U,
        "ETP EOMA exact size fields");

    failures += require(
        build_etp_abort(
            kRxAddress,
            kTxAddress,
            EtpAbortReason::timeout,
            kPayloadPgn,
            frame) &&
            decode_etp_cm(frame, cm) &&
            cm.control == EtpControl::abort &&
            cm.abort_reason ==
                static_cast<std::uint8_t>(
                    EtpAbortReason::timeout),
        "ETP Abort codec");

    std::array<std::byte, 7U> data{};
    data[0] = static_cast<std::byte>(0xA5U);
    failures += require(
        build_etp_dt(
            kTxAddress,
            kRxAddress,
            0x7FU,
            data,
            frame),
        "ETP DT builder");
    EtpDtFrame dt{};
    failures += require(
        decode_etp_dt(frame, dt) &&
            dt.sequence_number == 0x7FU &&
            dt.source_address == kTxAddress &&
            dt.destination_address == kRxAddress &&
            dt.data[0] == static_cast<std::byte>(0xA5U),
        "ETP DT codec");
  }

  {
    transport::CanFrame frame{};
    failures += require(
        !build_etp_rts(
            kTxAddress,
            kRxAddress,
            kEtpMinMessageBytes - 1U,
            kPayloadPgn,
            frame) &&
            build_etp_rts(
                kTxAddress,
                kRxAddress,
                kEtpMinMessageBytes,
                kPayloadPgn,
                frame) &&
            build_etp_rts(
                kTxAddress,
                kRxAddress,
                kEtpMaxMessageBytes,
                kPayloadPgn,
                frame) &&
            !build_etp_rts(
                kTxAddress,
                kRxAddress,
                kEtpMaxMessageBytes + 1U,
                kPayloadPgn,
                frame),
        "ETP 1786..111MiB size boundaries");

    failures += require(
        etp_packet_count(kEtpMinMessageBytes) == 256U &&
            etp_packet_count(kEtpMaxMessageBytes) ==
                kEtpMaxPacketCount,
        "ETP packet-count boundaries");

    failures += require(
        !build_etp_rts(
            kTxAddress,
            j1939::kGlobalAddress,
            kEtpMinMessageBytes,
            kPayloadPgn,
            frame),
        "ETP broadcast RTS rejected");

    failures += require(
        build_etp_dpo(
            kTxAddress,
            kRxAddress,
            1U,
            kEtpMaxPacketCount - 1U,
            kPayloadPgn,
            frame) &&
            !build_etp_dpo(
                kTxAddress,
                kRxAddress,
                2U,
                kEtpMaxPacketCount - 1U,
                kPayloadPgn,
                frame),
        "ETP DPO 24-bit packet-range boundaries");
  }

  {
    CountingSink sink;
    EtpReceiver receiver;
    failures += require(
        receiver.configure(receiver_config(), sink),
        "max-size receiver config");

    transport::CanFrame rts{};
    failures += require(
        build_etp_rts(
            kTxAddress,
            kRxAddress,
            kEtpMaxMessageBytes,
            kPayloadPgn,
            rts),
        "max-size RTS build");
    receiver.on_can_frame(wrap(rts, 0));

    transport::CanFrame cts{};
    EtpCmFrame cm{};
    failures += require(
        receiver.active() &&
            sink.begins == 1U &&
            sink.size == kEtpMaxMessageBytes &&
            receiver.try_take_tx(cts) &&
            decode_etp_cm(cts, cm) &&
            cm.control == EtpControl::cts &&
            cm.next_packet == 1U,
        "max-size session starts without allocating message buffer");
  }

  {
    CountingSink sink;
    sink.fail_begin = true;
    EtpReceiver receiver;
    failures += require(
        receiver.configure(receiver_config(), sink),
        "receiver begin failure config");

    transport::CanFrame rts{};
    (void)build_etp_rts(
        kTxAddress,
        kRxAddress,
        1800U,
        kPayloadPgn,
        rts);
    receiver.on_can_frame(wrap(rts, 0));

    transport::CanFrame abort{};
    EtpCmFrame cm{};
    failures += require(
        !receiver.active() &&
            receiver.counters().sink_failures == 1U &&
            receiver.try_take_tx(abort) &&
            decode_etp_cm(abort, cm) &&
            cm.control == EtpControl::abort &&
            cm.abort_reason ==
                static_cast<std::uint8_t>(
                    EtpAbortReason::resources),
        "sink begin failure sends resources Abort");
  }

  {
    BufferSink sink;
    EtpReceiver receiver;
    failures += require(
        receiver.configure(receiver_config(4U), sink),
        "receiver timeout config");

    transport::CanFrame rts{};
    (void)build_etp_rts(
        kTxAddress,
        kRxAddress,
        1800U,
        kPayloadPgn,
        rts);
    receiver.on_can_frame(wrap(rts, 0));

    failures += require(
        receiver.service_time(
            reading(EtpReceiver::kControlTimeout.count())) ==
                EtpReceiverStatus::no_action &&
            receiver.active(),
        "receiver timeout equality allowed");

    failures += require(
        receiver.service_time(
            reading(
                EtpReceiver::kControlTimeout.count() + 1)) ==
                EtpReceiverStatus::ok &&
            !receiver.active() &&
            sink.aborts == 1U &&
            receiver.counters().timeouts == 1U,
        "receiver timeout aborts streaming sink");
  }

  {
    PatternSource source(kEtpMaxMessageBytes);
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()),
        "max-size transmitter config");

    failures += require(
        transmitter.submit(
            EtpTransmitRequest{
                kPayloadPgn,
                kRxAddress,
                kEtpMaxMessageBytes},
            source,
            reading(0)) ==
                EtpTransmitterStatus::ok,
        "max-size streaming TX metadata accepted");

    transport::CanFrame rts{};
    EtpCmFrame cm{};
    failures += require(
        transmitter.try_take_tx(rts) &&
            decode_etp_cm(rts, cm) &&
            cm.control == EtpControl::rts &&
            cm.message_size == kEtpMaxMessageBytes &&
            source.reads == 0U,
        "max-size submit queues only metadata RTS");
  }

  {
    PatternSource source(1800U);
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()),
        "broadcast TX rejection config");
    failures += require(
        transmitter.submit(
            EtpTransmitRequest{
                kPayloadPgn,
                j1939::kGlobalAddress,
                1800U},
            source,
            reading(0)) ==
                EtpTransmitterStatus::invalid_argument,
        "ETP broadcast TX request rejected");
  }

  {
    PatternSource source(1800U);
    source.fail = true;
    source.fail_offset = 0U;
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()) &&
            transmitter.submit(
                EtpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    1800U},
                source,
                reading(0)) ==
                EtpTransmitterStatus::ok,
        "source failure setup");

    transport::CanFrame rts{};
    failures += require(
        transmitter.try_take_tx(rts),
        "drain source-failure RTS");

    transport::CanFrame cts{};
    (void)build_etp_cts(
        kRxAddress,
        kTxAddress,
        1U,
        1U,
        kPayloadPgn,
        cts);
    transmitter.on_can_frame(wrap(cts, 1000));

    transport::CanFrame dpo{};
    failures += require(
        transmitter.try_take_tx(dpo) &&
            is_control(
                dpo,
                EtpControl::dpo,
                kTxAddress,
                kRxAddress),
        "DPO emitted before source read");

    failures += require(
        transmitter.service_time(reading(2000)) ==
            EtpTransmitterStatus::ok,
        "source failure service returns after Abort queued");

    transport::CanFrame abort{};
    EtpCmFrame cm{};
    failures += require(
        !transmitter.active() &&
            transmitter.counters().source_failures == 1U &&
            transmitter.try_take_tx(abort) &&
            decode_etp_cm(abort, cm) &&
            cm.control == EtpControl::abort &&
            cm.abort_reason ==
                static_cast<std::uint8_t>(
                    EtpAbortReason::resources),
        "source failure sends resources Abort");
  }

  {
    PatternSource source(1800U);
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()) &&
            transmitter.submit(
                EtpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    1800U},
                source,
                reading(0)) ==
                EtpTransmitterStatus::ok,
        "transmitter timeout setup");
    transport::CanFrame rts{};
    failures += require(
        transmitter.try_take_tx(rts),
        "drain transmitter-timeout RTS");

    failures += require(
        transmitter.service_time(
            reading(EtpTransmitter::kResponseTimeout.count())) ==
                EtpTransmitterStatus::no_action &&
            transmitter.active(),
        "transmitter timeout equality allowed");

    failures += require(
        transmitter.service_time(
            reading(
                EtpTransmitter::kResponseTimeout.count() + 1)) ==
                EtpTransmitterStatus::ok &&
            !transmitter.active() &&
            transmitter.counters().timeouts == 1U,
        "transmitter timeout terminates session");

    transport::CanFrame abort{};
    EtpCmFrame cm{};
    failures += require(
        transmitter.try_take_tx(abort) &&
            decode_etp_cm(abort, cm) &&
            cm.control == EtpControl::abort &&
            cm.abort_reason ==
                static_cast<std::uint8_t>(
                    EtpAbortReason::timeout),
        "transmitter timeout sends timeout Abort");
  }

  {
    PatternSource source(1800U);
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()) &&
            transmitter.submit(
                EtpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    1800U},
                source,
                reading(100)) ==
                EtpTransmitterStatus::ok,
        "backward-time transmitter setup");
    failures += require(
        transmitter.service_time(reading(99)) ==
                EtpTransmitterStatus::invalid_time &&
            transmitter.status() ==
                EtpTransmitterStatus::protocol_fault &&
            !transmitter.active(),
        "backward transmitter time fails closed");
  }

  {
    BufferSink sink;
    EtpReceiver receiver;
    failures += require(
        receiver.configure(receiver_config(), sink),
        "backward-time receiver setup");

    transport::CanFrame rts{};
    (void)build_etp_rts(
        kTxAddress,
        kRxAddress,
        1800U,
        kPayloadPgn,
        rts);
    receiver.on_can_frame(wrap(rts, 100));

    failures += require(
        receiver.service_time(reading(99)) ==
                EtpReceiverStatus::invalid_time &&
            receiver.status() ==
                EtpReceiverStatus::protocol_fault &&
            !receiver.active() &&
            sink.aborts == 1U,
        "backward receiver time fails closed");
  }

  {
    BufferSink sink;
    EtpReceiver receiver;
    PatternSource source(1800U);
    EtpTransmitter transmitter;

    failures += require(
        receiver.configure(receiver_config(255U), sink) &&
            transmitter.configure(transmitter_config()) &&
            transmitter.submit(
                EtpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    1800U},
                source,
                reading(0)) ==
                EtpTransmitterStatus::ok,
        "1800-byte ETP end-to-end setup");

    std::int64_t now = 0;
    std::uint32_t dpo_count = 0U;
    std::uint32_t second_dpo_offset = 0U;
    std::uint32_t iterations = 0U;

    while ((transmitter.active() || receiver.active()) &&
           iterations < 2000U) {
      transport::CanFrame frame{};
      while (transmitter.try_take_tx(frame)) {
        ++now;
        EtpCmFrame cm{};
        if (decode_etp_cm(frame, cm) &&
            cm.control == EtpControl::dpo) {
          ++dpo_count;
          if (dpo_count == 2U) {
            second_dpo_offset = cm.packet_offset;
          }
        }
        receiver.on_can_frame(wrap(frame, now));
      }

      while (receiver.try_take_tx(frame)) {
        ++now;
        transmitter.on_can_frame(wrap(frame, now));
      }

      if (transmitter.active()) {
        ++now;
        const auto status =
            transmitter.service_time(reading(now));
        failures += require(
            status == EtpTransmitterStatus::ok ||
                status == EtpTransmitterStatus::no_action,
            "ETP transmitter E2E remains healthy");
      }

      if (receiver.active()) {
        ++now;
        const auto status = receiver.service_time(reading(now));
        failures += require(
            status == EtpReceiverStatus::ok ||
                status == EtpReceiverStatus::no_action,
            "ETP receiver E2E remains healthy");
      }
      ++iterations;
    }

    transport::CanFrame final_frame{};
    while (receiver.try_take_tx(final_frame)) {
      ++now;
      transmitter.on_can_frame(wrap(final_frame, now));
    }

    failures += require(
        iterations < 2000U &&
            !transmitter.active() &&
            !receiver.active() &&
            sink.committed &&
            sink.pattern_matches() &&
            sink.size == 1800U &&
            sink.pgn_value == kPayloadPgn &&
            sink.source == kTxAddress &&
            sink.destination == kRxAddress &&
            transmitter.counters().completed_messages == 1U &&
            receiver.counters().completed_messages == 1U &&
            dpo_count == 2U &&
            second_dpo_offset == 255U,
        "ETP 1800-byte streaming E2E crosses DPO offset 255");
  }

  {
    BufferSink sink;
    sink.fail_write = true;
    EtpReceiver receiver;
    failures += require(
        receiver.configure(receiver_config(1U), sink),
        "sink write failure setup");

    transport::CanFrame rts{};
    (void)build_etp_rts(
        kTxAddress,
        kRxAddress,
        1800U,
        kPayloadPgn,
        rts);
    receiver.on_can_frame(wrap(rts, 0));

    transport::CanFrame cts{};
    failures += require(
        receiver.try_take_tx(cts),
        "drain CTS for write failure");

    transport::CanFrame dpo{};
    (void)build_etp_dpo(
        kTxAddress,
        kRxAddress,
        1U,
        0U,
        kPayloadPgn,
        dpo);
    receiver.on_can_frame(wrap(dpo, 1));

    std::array<std::byte, 7U> data{};
    for (std::size_t i = 0U; i < data.size(); ++i) {
      data[i] = static_cast<std::byte>(
          static_cast<std::uint8_t>(0x31U + i));
    }
    transport::CanFrame dt{};
    (void)build_etp_dt(
        kTxAddress,
        kRxAddress,
        1U,
        data,
        dt);
    receiver.on_can_frame(wrap(dt, 2));

    transport::CanFrame abort{};
    EtpCmFrame cm{};
    failures += require(
        !receiver.active() &&
            sink.aborts == 1U &&
            receiver.counters().sink_failures == 1U &&
            receiver.try_take_tx(abort) &&
            decode_etp_cm(abort, cm) &&
            cm.control == EtpControl::abort &&
            cm.abort_reason ==
                static_cast<std::uint8_t>(
                    EtpAbortReason::resources),
        "sink write failure aborts ETP session");
  }

  {
    PatternSource source(1800U);
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()) &&
            transmitter.submit(
                EtpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    1800U},
                source,
                reading(0)) ==
                EtpTransmitterStatus::ok,
        "remote Abort setup");
    transport::CanFrame rts{};
    failures += require(
        transmitter.try_take_tx(rts),
        "drain remote-Abort RTS");

    transmitter.on_can_frame(
        remote_abort(kPayloadPgn, 1000));
    failures += require(
        !transmitter.active() &&
            transmitter.counters().remote_aborts == 1U,
        "remote ETP Abort terminates transmitter");
  }


  {
    PatternSource source(1800U);
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()) &&
            transmitter.submit(
                EtpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    1800U},
                source,
                reading(0)) ==
                EtpTransmitterStatus::ok,
        "ETP CTS hold setup");

    transport::CanFrame frame{};
    failures += require(
        transmitter.try_take_tx(frame),
        "drain ETP hold RTS");

    (void)build_etp_cts(
        kRxAddress,
        kTxAddress,
        0U,
        1U,
        kPayloadPgn,
        frame);
    transmitter.on_can_frame(wrap(frame, 100));

    failures += require(
        transmitter.service_time(
            reading(
                100 +
                EtpTransmitter::kHoldTimeout.count())) ==
                EtpTransmitterStatus::no_action &&
            transmitter.active(),
        "ETP CTS zero hold survives deadline equality");

    failures += require(
        transmitter.service_time(
            reading(
                101 +
                EtpTransmitter::kHoldTimeout.count())) ==
                EtpTransmitterStatus::ok &&
            !transmitter.active(),
        "ETP CTS zero hold times out fail-closed");
  }

  {
    PatternSource source(1800U);
    EtpTransmitter transmitter;
    failures += require(
        transmitter.configure(transmitter_config()) &&
            transmitter.submit(
                EtpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    1800U},
                source,
                reading(0)) ==
                EtpTransmitterStatus::ok,
        "ETP TX queue overflow setup");

    transport::CanFrame frame{};
    failures += require(
        transmitter.try_take_tx(frame),
        "drain ETP overflow RTS");

    (void)build_etp_cts(
        kRxAddress,
        kTxAddress,
        255U,
        1U,
        kPayloadPgn,
        frame);
    transmitter.on_can_frame(wrap(frame, 1));

    std::int64_t now = 2;
    while (transmitter.status() ==
               EtpTransmitterStatus::ok &&
           transmitter.active() &&
           now < 100) {
      (void)transmitter.service_time(reading(now));
      ++now;
    }

    failures += require(
        transmitter.status() ==
                EtpTransmitterStatus::queue_overflow &&
            transmitter.counters().tx_queue_overflows == 1U &&
            !transmitter.active(),
        "undrained ETP TX queue fails closed");
  }

  {
    BufferSink sink;
    EtpReceiver receiver;
    failures += require(
        receiver.configure(receiver_config(1U), sink),
        "ETP RX queue overflow setup");

    transport::CanFrame frame{};
    (void)build_etp_rts(
        kTxAddress,
        kRxAddress,
        1800U,
        kPayloadPgn,
        frame);
    receiver.on_can_frame(wrap(frame, 0));

    std::int64_t now = 1;
    for (std::uint32_t packet = 0U;
         packet < 16U &&
         receiver.status() == EtpReceiverStatus::ok &&
         receiver.active();
         ++packet) {
      (void)build_etp_dpo(
          kTxAddress,
          kRxAddress,
          1U,
          packet,
          kPayloadPgn,
          frame);
      receiver.on_can_frame(wrap(frame, now++));

      std::array<std::byte, 7U> data{};
      for (std::size_t i = 0U; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(
            static_cast<std::uint8_t>(
                ((packet * 7U) + i + 0x31U) & 0xFFU));
      }
      (void)build_etp_dt(
          kTxAddress,
          kRxAddress,
          1U,
          data,
          frame);
      receiver.on_can_frame(wrap(frame, now++));
    }

    failures += require(
        receiver.status() ==
                EtpReceiverStatus::queue_overflow &&
            receiver.counters().tx_queue_overflows == 1U &&
            !receiver.active() &&
            sink.aborts == 1U,
        "undrained ETP RX control queue fails closed");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_ISOBUS_ETP_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
