#include "ecu/core_v2/protocol/j1939/fd_transport_protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::j1939;

constexpr time::MonotonicClockDomainId kDomain{0x193922U};
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

FdTpReceiverConfig receiver_config(
    const std::uint8_t grant = 16U) {
  FdTpReceiverConfig config{};
  config.local_address = kRxAddress;
  config.max_segments_per_cts = grant;
  config.timestamp_domain = kDomain;
  config.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  return config;
}

FdTpTransmitterConfig transmitter_config(
    const std::uint8_t grant = 16U) {
  FdTpTransmitterConfig config{};
  config.local_address = kTxAddress;
  config.max_segments_per_cts = grant;
  config.timestamp_domain = kDomain;
  config.max_timestamp_uncertainty =
      time::MonotonicDuration{1};
  return config;
}

class PatternSource final : public IFdTpTransmitSource {
 public:
  explicit PatternSource(
      const std::uint32_t size) noexcept
      : size_(size) {}

  [[nodiscard]] bool read(
      const std::uint32_t byte_offset,
      std::array<std::byte, kFdTpSegmentPayloadBytes>& data,
      std::uint8_t& valid_bytes) noexcept override {
    ++reads;
    if (fail && byte_offset >= fail_offset) {
      return false;
    }
    if (byte_offset >= size_) {
      return false;
    }

    const auto remaining =
        size_ - byte_offset;
    valid_bytes =
        static_cast<std::uint8_t>(
            remaining <
                    static_cast<std::uint32_t>(
                        kFdTpSegmentPayloadBytes)
                ? remaining
                : static_cast<std::uint32_t>(
                      kFdTpSegmentPayloadBytes));
    for (std::uint8_t index = 0U;
         index < valid_bytes;
         ++index) {
      data[index] =
          static_cast<std::byte>(
              static_cast<std::uint8_t>(
                  (byte_offset +
                   static_cast<std::uint32_t>(index) +
                   0x31U) &
                  0xFFU));
    }
    return true;
  }

  bool fail{false};
  std::uint32_t fail_offset{0U};
  std::uint32_t reads{0U};

 private:
  std::uint32_t size_{0U};
};

class BufferSink final : public IFdTpReceiveSink {
 public:
  static constexpr std::size_t kCapacity = 4096U;

  [[nodiscard]] bool begin(
      const FdTpSessionKey& key_value,
      const std::uint32_t pgn,
      const std::uint32_t total_size,
      const bool broadcast_value) noexcept override {
    ++begins;
    if (fail_begin ||
        active ||
        total_size > buffer.size()) {
      return false;
    }

    key = key_value;
    pgn_value = pgn;
    size = total_size;
    written = 0U;
    broadcast = broadcast_value;
    active = true;
    committed = false;
    return true;
  }

  [[nodiscard]] bool write(
      const FdTpSessionKey& key_value,
      const std::uint32_t byte_offset,
      const std::array<std::byte, kFdTpSegmentPayloadBytes>& data,
      const std::uint8_t valid_bytes) noexcept override {
    ++writes;
    if (!active ||
        key_value != key ||
        fail_write ||
        valid_bytes == 0U ||
        valid_bytes > data.size() ||
        byte_offset != written ||
        byte_offset + valid_bytes > size ||
        byte_offset + valid_bytes > buffer.size()) {
      return false;
    }

    for (std::uint8_t index = 0U;
         index < valid_bytes;
         ++index) {
      buffer[
          byte_offset +
          static_cast<std::uint32_t>(index)] =
          data[index];
    }
    written += valid_bytes;
    return true;
  }

  [[nodiscard]] bool commit(
      const FdTpSessionKey& key_value) noexcept override {
    ++commits;
    if (!active ||
        key_value != key ||
        fail_commit ||
        written != size) {
      return false;
    }

    active = false;
    committed = true;
    return true;
  }

  void abort(
      const FdTpSessionKey& key_value) noexcept override {
    ++aborts;
    if (active && key_value == key) {
      active = false;
    }
  }

  [[nodiscard]] bool pattern_matches() const noexcept {
    if (!committed) {
      return false;
    }
    for (std::uint32_t index = 0U;
         index < size;
         ++index) {
      const auto expected =
          static_cast<std::byte>(
              static_cast<std::uint8_t>(
                  (index + 0x31U) & 0xFFU));
      if (buffer[index] != expected) {
        return false;
      }
    }
    return true;
  }

  std::array<std::byte, kCapacity> buffer{};
  FdTpSessionKey key{};
  std::uint32_t pgn_value{0U};
  std::uint32_t size{0U};
  std::uint32_t written{0U};
  std::uint32_t begins{0U};
  std::uint32_t writes{0U};
  std::uint32_t commits{0U};
  std::uint32_t aborts{0U};
  bool broadcast{false};
  bool active{false};
  bool committed{false};
  bool fail_begin{false};
  bool fail_write{false};
  bool fail_commit{false};
};

class CountingSink final : public IFdTpReceiveSink {
 public:
  [[nodiscard]] bool begin(
      const FdTpSessionKey&,
      const std::uint32_t,
      const std::uint32_t,
      const bool) noexcept override {
    ++begins;
    return !fail_begin;
  }

  [[nodiscard]] bool write(
      const FdTpSessionKey&,
      const std::uint32_t,
      const std::array<std::byte, kFdTpSegmentPayloadBytes>&,
      const std::uint8_t) noexcept override {
    ++writes;
    return true;
  }

  [[nodiscard]] bool commit(
      const FdTpSessionKey&) noexcept override {
    ++commits;
    return true;
  }

  void abort(
      const FdTpSessionKey&) noexcept override {
    ++aborts;
  }

  bool fail_begin{false};
  std::uint32_t begins{0U};
  std::uint32_t writes{0U};
  std::uint32_t commits{0U};
  std::uint32_t aborts{0U};
};

std::array<std::byte, kFdTpSegmentPayloadBytes>
pattern_segment(
    const std::uint32_t offset) {
  std::array<std::byte, kFdTpSegmentPayloadBytes>
      data{};
  for (std::size_t index = 0U;
       index < data.size();
       ++index) {
    data[index] =
        static_cast<std::byte>(
            static_cast<std::uint8_t>(
                (offset +
                 static_cast<std::uint32_t>(index) +
                 0x31U) &
                0xFFU));
  }
  return data;
}

bool is_cm(
    const transport::CanFrame& frame,
    const FdTpControl control,
    const std::uint8_t source,
    const std::uint8_t destination,
    const std::uint8_t session) {
  FdTpCmFrame cm{};
  return decode_fd_tp_cm(frame, cm) &&
         cm.control == control &&
         cm.key.source_address == source &&
         cm.key.destination_address == destination &&
         cm.key.session_number == session;
}

}  // namespace

int main() {
  int failures = 0;

  {
    failures += require(
        fd_tp_segment_count(61U) == 2U &&
            fd_tp_segment_count(120U) == 2U &&
            fd_tp_segment_count(121U) == 3U &&
            fd_tp_segment_count(
                kFdTpMaxMessageBytes) ==
                279621U,
        "FD.TP 60-byte segmentation math");
  }

  {
    transport::CanFrame frame{};
    failures += require(
        build_fd_tp_rts(
            3U,
            kTxAddress,
            kRxAddress,
            2U,
            121U,
            16U,
            kPayloadPgn,
            frame) &&
            frame.identifier == 0x0C4D8180U &&
            frame.identifier_format ==
                transport::CanIdentifierFormat::
                    extended_29_bit &&
            frame.format ==
                transport::CanFrameFormat::fd &&
            frame.bit_rate_switch &&
            frame.length == 12U,
        "FD.TP RTS envelope");

    const std::array<std::uint8_t, 12U> expected{
        0x20U,
        0x79U, 0x00U, 0x00U,
        0x03U, 0x00U, 0x00U,
        0x10U,
        0x00U,
        0x00U, 0xF1U, 0x00U};
    bool exact = true;
    for (std::size_t index = 0U;
         index < expected.size();
         ++index) {
      if (frame.payload[index] !=
          static_cast<std::byte>(
              expected[index])) {
        exact = false;
      }
    }
    failures += require(
        exact,
        "FD.TP RTS 12-byte wire layout");

    FdTpCmFrame cm{};
    failures += require(
        decode_fd_tp_cm(frame, cm) &&
            cm.control == FdTpControl::rts &&
            cm.key.session_number == 2U &&
            cm.key.source_address == kTxAddress &&
            cm.key.destination_address == kRxAddress &&
            cm.priority == 3U &&
            cm.message_size == 121U &&
            cm.segment_number == 3U &&
            cm.parameter7 == 16U &&
            cm.parameter8 == 0U &&
            cm.transported_pgn == kPayloadPgn,
        "FD.TP RTS decode");
  }

  {
    transport::CanFrame frame{};
    FdTpCmFrame cm{};

    failures += require(
        build_fd_tp_bam(
            6U,
            kTxAddress,
            1U,
            121U,
            kPayloadPgn,
            frame) &&
            decode_fd_tp_cm(frame, cm) &&
            cm.control == FdTpControl::bam &&
            cm.key.destination_address ==
                kGlobalAddress &&
            cm.message_size == 121U &&
            cm.segment_number == 3U &&
            cm.parameter7 == 0xFFU &&
            cm.parameter8 == 0U,
        "FD.TP BAM codec");

    failures += require(
        build_fd_tp_cts(
            kRxAddress,
            kTxAddress,
            2U,
            7U,
            1000U,
            kPayloadPgn,
            frame) &&
            decode_fd_tp_cm(frame, cm) &&
            cm.control == FdTpControl::cts &&
            cm.message_size == 0xFFFFFFU &&
            cm.segment_number == 1000U &&
            cm.parameter7 == 7U &&
            cm.parameter8 == 0U,
        "FD.TP CTS codec");

    failures += require(
        build_fd_tp_end_of_message_status(
            kTxAddress,
            kRxAddress,
            2U,
            121U,
            kPayloadPgn,
            frame) &&
            decode_fd_tp_cm(frame, cm) &&
            cm.control ==
                FdTpControl::end_of_message_status &&
            cm.message_size == 121U &&
            cm.segment_number == 3U &&
            cm.parameter7 == 0U &&
            cm.parameter8 == 0U,
        "FD.TP EOMS codec");

    failures += require(
        build_fd_tp_end_of_message_ack(
            kRxAddress,
            kTxAddress,
            2U,
            121U,
            kPayloadPgn,
            frame) &&
            decode_fd_tp_cm(frame, cm) &&
            cm.control ==
                FdTpControl::end_of_message_ack &&
            cm.parameter7 == 0xFFU &&
            cm.parameter8 == 0xFFU,
        "FD.TP EOMA codec");

    failures += require(
        build_fd_tp_abort(
            kRxAddress,
            kTxAddress,
            2U,
            FdTpAbortReason::timeout,
            kPayloadPgn,
            frame) &&
            decode_fd_tp_cm(frame, cm) &&
            cm.control == FdTpControl::abort &&
            cm.message_size == 0xFFFFFFU &&
            cm.segment_number == 0xFFFFFFU &&
            cm.parameter7 == 0xFFU &&
            cm.parameter8 ==
                static_cast<std::uint8_t>(
                    FdTpAbortReason::timeout),
        "FD.TP Abort codec");
  }

  {
    auto data = pattern_segment(0U);
    transport::CanFrame frame{};
    failures += require(
        build_fd_tp_dt(
            kTxAddress,
            kRxAddress,
            3U,
            1U,
            data,
            60U,
            frame) &&
            frame.length == 64U,
        "full FD.TP.DT uses 64-byte frame");

    FdTpDtFrame dt{};
    failures += require(
        decode_fd_tp_dt(frame, dt) &&
            dt.key.session_number == 3U &&
            dt.key.source_address == kTxAddress &&
            dt.key.destination_address == kRxAddress &&
            dt.dtfi == 0U &&
            dt.segment_number == 1U &&
            dt.wire_data_bytes == 60U &&
            dt.data[0U] == std::byte{0x31U},
        "full FD.TP.DT codec");

    data = pattern_segment(120U);
    failures += require(
        build_fd_tp_dt(
            kTxAddress,
            kRxAddress,
            3U,
            3U,
            data,
            5U,
            frame) &&
            frame.length == 12U &&
            frame.payload[9U] ==
                std::byte{0xFFU} &&
            frame.payload[11U] ==
                std::byte{0xFFU} &&
            decode_fd_tp_dt(frame, dt) &&
            dt.wire_data_bytes == 8U,
        "short final FD.TP.DT uses legal DLC and FF padding");

    frame.payload[0U] =
        static_cast<std::byte>(
            std::to_integer<std::uint8_t>(
                frame.payload[0U]) |
            0x01U);
    failures += require(
        !decode_fd_tp_dt(frame, dt),
        "unsupported DTFI fails closed");
  }

  {
    transport::CanFrame frame{};
    failures += require(
        !build_fd_tp_rts(
            3U,
            kTxAddress,
            kRxAddress,
            0U,
            60U,
            16U,
            kPayloadPgn,
            frame) &&
            build_fd_tp_rts(
                3U,
                kTxAddress,
                kRxAddress,
                0U,
                61U,
                16U,
                kPayloadPgn,
                frame) &&
            build_fd_tp_rts(
                3U,
                kTxAddress,
                kRxAddress,
                0U,
                kFdTpMaxMessageBytes,
                16U,
                kPayloadPgn,
                frame),
        "FD.TP peer size boundaries");

    failures += require(
        build_fd_tp_bam(
            6U,
            kTxAddress,
            0U,
            kFdTpMaxBamMessageBytes,
            kPayloadPgn,
            frame) &&
            !build_fd_tp_bam(
                6U,
                kTxAddress,
                0U,
                kFdTpMaxBamMessageBytes + 1U,
                kPayloadPgn,
                frame),
        "FD.TP BAM 15300-byte boundary");

    failures += require(
        !build_fd_tp_rts(
            3U,
            kTxAddress,
            kGlobalAddress,
            0U,
            121U,
            16U,
            kPayloadPgn,
            frame) &&
            !build_fd_tp_bam(
                6U,
                kNullAddress,
                0U,
                121U,
                kPayloadPgn,
                frame),
        "FD.TP addressed/global source boundaries");
  }

  {
    BufferSink sink;
    FdTpReceiver receiver;
    failures += require(
        receiver.configure(
            receiver_config(),
            sink),
        "BAM receiver config");

    transport::CanFrame frame{};
    (void)build_fd_tp_bam(
        6U,
        kTxAddress,
        1U,
        121U,
        kPayloadPgn,
        frame);
    receiver.on_can_frame(wrap(frame, 0));

    failures += require(
        receiver.active_session_count() == 1U &&
            sink.begins == 1U &&
            sink.broadcast,
        "BAM receiver opens streaming sink");

    for (std::uint32_t segment = 1U;
         segment <= 3U;
         ++segment) {
      const auto offset =
          (segment - 1U) * 60U;
      const auto data =
          pattern_segment(offset);
      const std::uint8_t valid =
          segment == 3U
              ? static_cast<std::uint8_t>(1U)
              : static_cast<std::uint8_t>(60U);
      (void)build_fd_tp_dt(
          kTxAddress,
          kGlobalAddress,
          1U,
          segment,
          data,
          valid,
          frame);
      receiver.on_can_frame(
          wrap(
              frame,
              static_cast<std::int64_t>(
                  segment) *
                  10000000LL));
    }

    failures += require(
        receiver.active_session_count() == 1U &&
            sink.written == 121U &&
            !sink.committed,
        "BAM data waits for EOMS before commit");

    (void)build_fd_tp_end_of_message_status(
        kTxAddress,
        kGlobalAddress,
        1U,
        121U,
        kPayloadPgn,
        frame);
    receiver.on_can_frame(
        wrap(frame, 40000000LL));

    failures += require(
        receiver.active_session_count() == 0U &&
            sink.committed &&
            sink.pattern_matches() &&
            receiver.counters().completed_messages == 1U &&
            receiver.pending_tx_count() == 0U,
        "BAM EOMS commits streaming payload without EOMA");
  }

  {
    BufferSink sink;
    FdTpReceiver receiver;
    failures += require(
        receiver.configure(
            receiver_config(2U),
            sink),
        "RTS/CTS receiver config");

    transport::CanFrame frame{};
    (void)build_fd_tp_rts(
        3U,
        kTxAddress,
        kRxAddress,
        2U,
        121U,
        8U,
        kPayloadPgn,
        frame);
    receiver.on_can_frame(wrap(frame, 0));

    transport::CanFrame cts{};
    FdTpCmFrame cm{};
    failures += require(
        receiver.try_take_tx(cts) &&
            decode_fd_tp_cm(cts, cm) &&
            cm.control == FdTpControl::cts &&
            cm.key.session_number == 2U &&
            cm.parameter7 == 2U &&
            cm.segment_number == 1U,
        "receiver grants first two FD.TP segments");

    for (std::uint32_t segment = 1U;
         segment <= 2U;
         ++segment) {
      const auto data =
          pattern_segment(
              (segment - 1U) * 60U);
      (void)build_fd_tp_dt(
          kTxAddress,
          kRxAddress,
          2U,
          segment,
          data,
          60U,
          frame);
      receiver.on_can_frame(
          wrap(
              frame,
              static_cast<std::int64_t>(
                  segment) *
                  1000LL));
    }

    failures += require(
        receiver.try_take_tx(cts) &&
            decode_fd_tp_cm(cts, cm) &&
            cm.control == FdTpControl::cts &&
            cm.parameter7 == 1U &&
            cm.segment_number == 3U,
        "receiver grants final FD.TP segment");

    const auto final_data =
        pattern_segment(120U);
    (void)build_fd_tp_dt(
        kTxAddress,
        kRxAddress,
        2U,
        3U,
        final_data,
        1U,
        frame);
    receiver.on_can_frame(wrap(frame, 3000));

    (void)build_fd_tp_end_of_message_status(
        kTxAddress,
        kRxAddress,
        2U,
        121U,
        kPayloadPgn,
        frame);
    receiver.on_can_frame(wrap(frame, 4000));

    transport::CanFrame eoma{};
    failures += require(
        receiver.try_take_tx(eoma) &&
            is_cm(
                eoma,
                FdTpControl::end_of_message_ack,
                kRxAddress,
                kTxAddress,
                2U) &&
            sink.committed &&
            sink.pattern_matches() &&
            receiver.active_session_count() == 0U,
        "RTS/CTS receiver commits and emits EOMA");
  }

  {
    CountingSink sink;
    FdTpReceiver receiver;
    failures += require(
        receiver.configure(
            receiver_config(),
            sink),
        "parallel BAM receiver config");

    for (std::uint8_t index = 0U;
         index < 4U;
         ++index) {
      transport::CanFrame bam{};
      (void)build_fd_tp_bam(
          6U,
          static_cast<std::uint8_t>(
              0x80U + index),
          index,
          121U,
          kPayloadPgn,
          bam);
      receiver.on_can_frame(
          wrap(
              bam,
              static_cast<std::int64_t>(
                  index)));
    }

    failures += require(
        receiver.active_session_count() == 4U &&
            sink.begins == 4U,
        "receiver holds four concurrent BAM sessions");

    transport::CanFrame fifth{};
    (void)build_fd_tp_bam(
        6U,
        0x90U,
        4U,
        121U,
        kPayloadPgn,
        fifth);
    receiver.on_can_frame(wrap(fifth, 10));
    failures += require(
        receiver.active_session_count() == 4U &&
            receiver.counters().rejected_sessions == 1U,
        "fifth concurrent BAM session rejected boundedly");
  }

  {
    CountingSink sink;
    FdTpReceiver receiver;
    failures += require(
        receiver.configure(
            receiver_config(),
            sink),
        "parallel peer receiver config");

    for (std::uint8_t index = 0U;
         index < 8U;
         ++index) {
      transport::CanFrame rts{};
      (void)build_fd_tp_rts(
          3U,
          kTxAddress,
          kRxAddress,
          index,
          121U,
          2U,
          static_cast<std::uint32_t>(
              kPayloadPgn +
              static_cast<std::uint32_t>(index)),
          rts);
      receiver.on_can_frame(
          wrap(
              rts,
              static_cast<std::int64_t>(
                  index)));
    }

    failures += require(
        receiver.active_session_count() == 8U &&
            sink.begins == 8U &&
            receiver.pending_tx_count() == 8U,
        "receiver holds eight concurrent peer sessions");

    transport::CanFrame ninth{};
    (void)build_fd_tp_rts(
        3U,
        0x82U,
        kRxAddress,
        8U,
        121U,
        2U,
        0xF120U,
        ninth);
    receiver.on_can_frame(wrap(ninth, 20));

    failures += require(
        receiver.active_session_count() == 8U &&
            receiver.counters().rejected_sessions == 1U &&
            receiver.pending_tx_count() == 9U,
        "ninth peer session rejected with bounded Abort");
  }

  {
    PatternSource source(121U);
    FdTpTransmitter transmitter;
    failures += require(
        transmitter.configure(
            transmitter_config()) &&
            transmitter.submit(
                FdTpTransmitRequest{
                    kPayloadPgn,
                    kGlobalAddress,
                    121U,
                    6U},
                source,
                reading(0)) ==
                FdTpTransmitterStatus::ok,
        "BAM transmitter submit");

    transport::CanFrame frame{};
    failures += require(
        transmitter.try_take_tx(frame) &&
            is_cm(
                frame,
                FdTpControl::bam,
                kTxAddress,
                kGlobalAddress,
                0U) &&
            source.reads == 0U,
        "BAM submit queues metadata before source reads");

    failures += require(
        transmitter.service_time(
            reading(
                FdTpTransmitter::
                    kBamSegmentInterval.count())) ==
            FdTpTransmitterStatus::ok,
        "BAM first segment scheduled at 10ms");
    failures += require(
        transmitter.try_take_tx(frame),
        "BAM first DT queued");

    FdTpDtFrame dt{};
    failures += require(
        decode_fd_tp_dt(frame, dt) &&
            dt.segment_number == 1U,
        "BAM first DT segment");

    failures += require(
        transmitter.service_time(
            reading(
                2 *
                FdTpTransmitter::
                    kBamSegmentInterval.count())) ==
            FdTpTransmitterStatus::ok &&
            transmitter.try_take_tx(frame) &&
            decode_fd_tp_dt(frame, dt) &&
            dt.segment_number == 2U,
        "BAM second DT segment");

    failures += require(
        transmitter.service_time(
            reading(
                3 *
                FdTpTransmitter::
                    kBamSegmentInterval.count())) ==
            FdTpTransmitterStatus::ok &&
            transmitter.try_take_tx(frame) &&
            decode_fd_tp_dt(frame, dt) &&
            dt.segment_number == 3U,
        "BAM final DT segment");

    failures += require(
        transmitter.service_time(
            reading(
                4 *
                FdTpTransmitter::
                    kBamSegmentInterval.count())) ==
            FdTpTransmitterStatus::ok &&
            transmitter.try_take_tx(frame) &&
            is_cm(
                frame,
                FdTpControl::end_of_message_status,
                kTxAddress,
                kGlobalAddress,
                0U) &&
            transmitter.active_session_count() == 0U &&
            transmitter.counters().completed_messages == 1U &&
            source.reads == 3U,
        "BAM EOMS completes sender");
  }

  {
    PatternSource source(kFdTpMaxMessageBytes);
    FdTpTransmitter transmitter;
    failures += require(
        transmitter.configure(
            transmitter_config()) &&
            transmitter.submit(
                FdTpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    kFdTpMaxMessageBytes,
                    3U},
                source,
                reading(0)) ==
                FdTpTransmitterStatus::ok,
        "max 24-bit peer transfer metadata accepted");

    transport::CanFrame rts{};
    FdTpCmFrame cm{};
    failures += require(
        transmitter.try_take_tx(rts) &&
            decode_fd_tp_cm(rts, cm) &&
            cm.control == FdTpControl::rts &&
            cm.message_size ==
                kFdTpMaxMessageBytes &&
            cm.segment_number == 279621U &&
            source.reads == 0U,
        "max-size submit allocates no message buffer and performs no source read");
  }

  {
    PatternSource source(121U);
    BufferSink sink;
    FdTpTransmitter transmitter;
    FdTpReceiver receiver;

    failures += require(
        transmitter.configure(
            transmitter_config(2U)) &&
            receiver.configure(
                receiver_config(2U),
                sink) &&
            transmitter.submit(
                FdTpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    121U,
                    3U},
                source,
                reading(0)) ==
                FdTpTransmitterStatus::ok,
        "FD.TP peer end-to-end setup");

    std::int64_t now = 0;
    std::uint32_t iterations = 0U;
    while ((transmitter.active_session_count() != 0U ||
            receiver.active_session_count() != 0U) &&
           iterations < 100U) {
      transport::CanFrame frame{};
      while (transmitter.try_take_tx(frame)) {
        ++now;
        receiver.on_can_frame(wrap(frame, now));
      }
      while (receiver.try_take_tx(frame)) {
        ++now;
        transmitter.on_can_frame(wrap(frame, now));
      }

      if (transmitter.active_session_count() != 0U) {
        ++now;
        const auto status =
            transmitter.service_time(
                reading(now));
        failures += require(
            status ==
                    FdTpTransmitterStatus::ok ||
                status ==
                    FdTpTransmitterStatus::
                        no_action,
            "peer transmitter remains healthy");
      }
      if (receiver.active_session_count() != 0U) {
        ++now;
        const auto status =
            receiver.service_time(
                reading(now));
        failures += require(
            status ==
                    FdTpReceiverStatus::ok ||
                status ==
                    FdTpReceiverStatus::
                        no_action,
            "peer receiver remains healthy");
      }
      ++iterations;
    }

    transport::CanFrame tail{};
    while (receiver.try_take_tx(tail)) {
      ++now;
      transmitter.on_can_frame(
          wrap(tail, now));
    }

    failures += require(
        iterations < 100U &&
            transmitter.active_session_count() == 0U &&
            receiver.active_session_count() == 0U &&
            source.reads == 3U &&
            sink.committed &&
            sink.pattern_matches() &&
            transmitter.counters().completed_messages == 1U &&
            receiver.counters().completed_messages == 1U,
        "121-byte RTS/CTS FD.TP streaming end-to-end");
  }

  {
    PatternSource sources[5] = {
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U}};
    FdTpTransmitter transmitter;
    failures += require(
        transmitter.configure(
            transmitter_config()),
        "parallel BAM transmitter config");

    for (std::size_t index = 0U;
         index < 4U;
         ++index) {
      failures += require(
          transmitter.submit(
              FdTpTransmitRequest{
                  static_cast<std::uint32_t>(
                      kPayloadPgn + index),
                  kGlobalAddress,
                  121U,
                  6U},
              sources[index],
              reading(
                  static_cast<std::int64_t>(
                      index))) ==
              FdTpTransmitterStatus::ok,
          "parallel BAM submit");
    }

    failures += require(
        transmitter.active_session_count() == 4U &&
            transmitter.submit(
                FdTpTransmitRequest{
                    0xF110U,
                    kGlobalAddress,
                    121U,
                    6U},
                sources[4U],
                reading(10)) ==
                FdTpTransmitterStatus::busy,
        "fifth BAM TX session rejected boundedly");
  }

  {
    std::array<PatternSource, 9U> sources{
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U},
        PatternSource{121U}};
    FdTpTransmitter transmitter;
    failures += require(
        transmitter.configure(
            transmitter_config()),
        "parallel peer transmitter config");

    for (std::size_t index = 0U;
         index < 8U;
         ++index) {
      failures += require(
          transmitter.submit(
              FdTpTransmitRequest{
                  static_cast<std::uint32_t>(
                      kPayloadPgn + index),
                  static_cast<std::uint8_t>(
                      kRxAddress + index),
                  121U,
                  3U},
              sources[index],
              reading(
                  static_cast<std::int64_t>(
                      index))) ==
              FdTpTransmitterStatus::ok,
          "parallel peer submit");
    }

    failures += require(
        transmitter.active_session_count() == 8U &&
            transmitter.submit(
                FdTpTransmitRequest{
                    0xF120U,
                    0x90U,
                    121U,
                    3U},
                sources[8U],
                reading(10)) ==
                FdTpTransmitterStatus::busy,
        "ninth peer TX session rejected boundedly");
  }

  {
    PatternSource source(121U);
    FdTpTransmitter transmitter;
    failures += require(
        transmitter.configure(
            transmitter_config(2U)) &&
            transmitter.submit(
                FdTpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    121U,
                    3U},
                source,
                reading(0)) ==
                FdTpTransmitterStatus::ok,
        "CTS-during-DT setup");

    transport::CanFrame frame{};
    failures += require(
        transmitter.try_take_tx(frame),
        "drain RTS");

    (void)build_fd_tp_cts(
        kRxAddress,
        kTxAddress,
        0U,
        2U,
        1U,
        kPayloadPgn,
        frame);
    transmitter.on_can_frame(wrap(frame, 1));
    failures += require(
        transmitter.service_time(
            reading(2)) ==
            FdTpTransmitterStatus::ok &&
            transmitter.try_take_tx(frame),
        "first DT queued after CTS");

    (void)build_fd_tp_cts(
        kRxAddress,
        kTxAddress,
        0U,
        1U,
        2U,
        kPayloadPgn,
        frame);
    transmitter.on_can_frame(wrap(frame, 3));

    transport::CanFrame abort{};
    FdTpCmFrame cm{};
    failures += require(
        transmitter.active_session_count() == 0U &&
            transmitter.try_take_tx(abort) &&
            decode_fd_tp_cm(abort, cm) &&
            cm.control == FdTpControl::abort &&
            cm.parameter8 ==
                static_cast<std::uint8_t>(
                    FdTpAbortReason::
                        cts_while_data_transfer),
        "CTS while data transfer aborts session fail-closed");
  }

  {
    CountingSink sink;
    FdTpReceiver receiver;
    failures += require(
        receiver.configure(
            receiver_config(),
            sink),
        "receiver timeout setup");

    transport::CanFrame rts{};
    (void)build_fd_tp_rts(
        3U,
        kTxAddress,
        kRxAddress,
        0U,
        121U,
        2U,
        kPayloadPgn,
        rts);
    receiver.on_can_frame(wrap(rts, 0));

    failures += require(
        receiver.service_time(
            reading(
                FdTpReceiver::
                    kCtsDataTimeout.count())) ==
                FdTpReceiverStatus::no_action &&
            receiver.active_session_count() == 1U,
        "receiver timeout equality allowed");

    failures += require(
        receiver.service_time(
            reading(
                FdTpReceiver::
                    kCtsDataTimeout.count() + 2)) ==
                FdTpReceiverStatus::ok &&
            receiver.active_session_count() == 0U &&
            sink.aborts == 1U &&
            receiver.counters().timeouts == 1U,
        "receiver peer timeout aborts sink and session");
  }

  {
    PatternSource source(121U);
    FdTpTransmitter transmitter;
    failures += require(
        transmitter.configure(
            transmitter_config()) &&
            transmitter.submit(
                FdTpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    121U,
                    3U},
                source,
                reading(0)) ==
                FdTpTransmitterStatus::ok,
        "transmitter timeout setup");
    transport::CanFrame rts{};
    failures += require(
        transmitter.try_take_tx(rts),
        "drain timeout RTS");

    failures += require(
        transmitter.service_time(
            reading(
                FdTpTransmitter::
                    kResponseTimeout.count())) ==
                FdTpTransmitterStatus::no_action &&
            transmitter.active_session_count() == 1U,
        "transmitter timeout equality allowed");

    failures += require(
        transmitter.service_time(
            reading(
                FdTpTransmitter::
                    kResponseTimeout.count() + 2)) ==
                FdTpTransmitterStatus::ok &&
            transmitter.active_session_count() == 0U &&
            transmitter.counters().timeouts == 1U,
        "transmitter response timeout terminates peer session");

    transport::CanFrame abort{};
    FdTpCmFrame cm{};
    failures += require(
        transmitter.try_take_tx(abort) &&
            decode_fd_tp_cm(abort, cm) &&
            cm.control == FdTpControl::abort &&
            cm.parameter8 ==
                static_cast<std::uint8_t>(
                    FdTpAbortReason::timeout),
        "transmitter timeout emits Abort");
  }

  {
    CountingSink sink;
    FdTpReceiver receiver;
    failures += require(
        receiver.configure(
            receiver_config(),
            sink),
        "receiver backward-time setup");

    transport::CanFrame bam{};
    (void)build_fd_tp_bam(
        6U,
        kTxAddress,
        0U,
        121U,
        kPayloadPgn,
        bam);
    receiver.on_can_frame(wrap(bam, 100));

    failures += require(
        receiver.service_time(
            reading(99)) ==
                FdTpReceiverStatus::invalid_time &&
            receiver.status() ==
                FdTpReceiverStatus::protocol_fault &&
            receiver.active_session_count() == 0U &&
            sink.aborts == 1U,
        "receiver backward time fails closed");
  }

  {
    PatternSource source(121U);
    FdTpTransmitter transmitter;
    failures += require(
        transmitter.configure(
            transmitter_config()) &&
            transmitter.submit(
                FdTpTransmitRequest{
                    kPayloadPgn,
                    kRxAddress,
                    121U,
                    3U},
                source,
                reading(100)) ==
                FdTpTransmitterStatus::ok,
        "transmitter backward-time setup");

    failures += require(
        transmitter.service_time(
            reading(99)) ==
                FdTpTransmitterStatus::invalid_time &&
            transmitter.status() ==
                FdTpTransmitterStatus::protocol_fault &&
            transmitter.active_session_count() == 0U,
        "transmitter backward time fails closed");
  }

  {
    CountingSink sink;
    FdTpReceiver receiver;
    failures += require(
        receiver.configure(
            receiver_config(),
            sink),
        "unsupported assurance setup");

    transport::CanFrame rts{};
    (void)build_fd_tp_rts(
        3U,
        kTxAddress,
        kRxAddress,
        0U,
        121U,
        2U,
        kPayloadPgn,
        rts);
    rts.payload[8U] = std::byte{1U};
    receiver.on_can_frame(wrap(rts, 0));
    failures += require(
        receiver.active_session_count() == 0U &&
            receiver.counters().malformed_frames == 1U,
        "nonzero FD.TP assurance type is not silently accepted");
  }

  if (failures == 0) {
    std::cout
        << "CORE_V2_J1939_FD_TP_TESTS=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
