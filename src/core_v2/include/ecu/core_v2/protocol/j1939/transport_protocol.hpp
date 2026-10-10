#pragma once

#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

inline constexpr std::uint32_t kTpCmPgn = 0xEC00U;
inline constexpr std::uint32_t kTpDtPgn = 0xEB00U;
inline constexpr std::size_t kTpPacketPayloadBytes = 7U;
inline constexpr std::size_t kTpMaxPackets = 255U;
inline constexpr std::size_t kTpMaxMessageBytes =
    kTpPacketPayloadBytes * kTpMaxPackets;

enum class TpControl : std::uint8_t {
  rts = 0x10U,
  cts = 0x11U,
  end_of_message_ack = 0x13U,
  bam = 0x20U,
  abort = 0xFFU,
};

enum class TpAbortReason : std::uint8_t {
  already_in_session = 1U,
  resources = 2U,
  timeout = 3U,
};

struct TpCmFrame {
  TpControl control{TpControl::abort};
  std::uint8_t source_address{0U};
  std::uint8_t destination_address{0U};
  std::uint16_t message_size{0U};
  std::uint8_t packet_count{0U};
  std::uint8_t control_parameter{0U};
  std::uint32_t transported_pgn{0U};
};

struct TpDtFrame {
  std::uint8_t source_address{0U};
  std::uint8_t destination_address{0U};
  std::uint8_t sequence_number{0U};
  std::array<std::byte, kTpPacketPayloadBytes> data{};
};

[[nodiscard]] bool decode_tp_cm(
    const transport::CanFrame& frame,
    TpCmFrame& value) noexcept;

[[nodiscard]] bool decode_tp_dt(
    const transport::CanFrame& frame,
    TpDtFrame& value) noexcept;

[[nodiscard]] bool build_tp_bam(
    std::uint8_t source_address,
    std::uint16_t message_size,
    std::uint8_t packet_count,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_tp_rts(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint16_t message_size,
    std::uint8_t packet_count,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_tp_dt(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t sequence_number,
    const std::array<std::byte, kTpPacketPayloadBytes>& data,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_tp_cts(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t packets_allowed,
    std::uint8_t next_packet,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_tp_end_of_message_ack(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint16_t message_size,
    std::uint8_t packet_count,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_tp_abort(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    TpAbortReason reason,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

struct TpMessage {
  std::uint32_t pgn{0U};
  std::uint8_t source_address{0U};
  std::uint8_t destination_address{kGlobalAddress};
  std::uint16_t size{0U};
  bool broadcast{false};
  std::array<std::byte, kTpMaxMessageBytes> data{};
};

struct TpReceiverConfig {
  std::uint8_t local_address{kNullAddress};
  std::uint8_t max_packets_per_cts{16U};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
};

enum class TpReceiverStatus : std::uint8_t {
  ok,
  no_action,
  invalid_state,
  invalid_time,
  queue_overflow,
  protocol_fault,
};

struct TpReceiverCounters {
  std::uint32_t cm_frames{0U};
  std::uint32_t dt_frames{0U};
  std::uint32_t bam_sessions_started{0U};
  std::uint32_t peer_sessions_started{0U};
  std::uint32_t completed_messages{0U};
  std::uint32_t malformed_frames{0U};
  std::uint32_t rejected_sessions{0U};
  std::uint32_t timeouts{0U};
  std::uint32_t tx_queue_overflows{0U};
};

class TpReceiver final : public transport::ICanFrameSink {
 public:
  static constexpr std::size_t kTxQueueCapacity = 8U;
  static constexpr std::size_t kMessageQueueCapacity = 2U;

  inline static constexpr time::MonotonicDuration
      kPacketTimeout{750000000};
  inline static constexpr time::MonotonicDuration
      kCtsDataTimeout{1250000000};

  [[nodiscard]] bool configure(
      const TpReceiverConfig& config) noexcept;
  [[nodiscard]] bool set_local_address(
      std::uint8_t address) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] TpReceiverStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;
  [[nodiscard]] bool try_take_message(
      TpMessage& message) noexcept;

  [[nodiscard]] TpReceiverStatus status() const noexcept;
  [[nodiscard]] TpReceiverCounters counters() const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;
  [[nodiscard]] std::size_t pending_message_count() const noexcept;
  [[nodiscard]] bool bam_active() const noexcept;
  [[nodiscard]] bool peer_active() const noexcept;

 private:
  struct RxSession {
    bool active{false};
    bool broadcast{false};
    std::uint8_t source_address{0U};
    std::uint8_t destination_address{kGlobalAddress};
    std::uint32_t pgn{0U};
    std::uint16_t message_size{0U};
    std::uint8_t packet_count{0U};
    std::uint8_t next_sequence{1U};
    std::uint8_t granted_packets_remaining{0U};
    std::uint16_t bytes_received{0U};
    time::MonotonicTime deadline{0};
    std::array<std::byte, kTpMaxMessageBytes> data{};
  };

  [[nodiscard]] bool valid_config(
      const TpReceiverConfig& config) const noexcept;
  [[nodiscard]] bool observe_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;

  void handle_cm(
      const TpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void handle_dt(
      const TpDtFrame& dt,
      const time::MonotonicClockReading& timestamp) noexcept;
  void start_bam(
      const TpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void start_peer(
      const TpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void accept_dt(
      RxSession& session,
      const TpDtFrame& dt,
      const time::MonotonicClockReading& timestamp) noexcept;
  void complete_session(RxSession& session) noexcept;
  void reset_session(RxSession& session) noexcept;
  void timeout_peer() noexcept;

  [[nodiscard]] bool queue_cts(
      const RxSession& session,
      std::uint8_t packets_allowed,
      std::uint8_t next_packet) noexcept;
  [[nodiscard]] bool queue_eom_ack(
      const RxSession& session) noexcept;
  [[nodiscard]] bool queue_abort(
      std::uint8_t destination,
      TpAbortReason reason,
      std::uint32_t pgn) noexcept;
  [[nodiscard]] bool enqueue_tx(
      const transport::CanFrame& frame) noexcept;
  [[nodiscard]] bool enqueue_message(
      const RxSession& session) noexcept;
  [[nodiscard]] bool has_reserved_message_slot() const noexcept;
  void latch_fault() noexcept;

  TpReceiverConfig config_{};
  RxSession bam_{};
  RxSession peer_{};
  std::array<transport::CanFrame, kTxQueueCapacity> tx_queue_{};
  std::size_t tx_head_{0U};
  std::size_t tx_tail_{0U};
  std::size_t tx_count_{0U};
  std::array<TpMessage, kMessageQueueCapacity> message_queue_{};
  std::size_t message_head_{0U};
  std::size_t message_tail_{0U};
  std::size_t message_count_{0U};
  time::MonotonicTime last_observed_time_{0};
  bool has_last_observed_time_{false};
  bool configured_{false};
  TpReceiverStatus status_{TpReceiverStatus::invalid_state};
  TpReceiverCounters counters_{};
};

struct TpTransmitMessage {
  std::uint32_t pgn{0U};
  std::uint8_t destination_address{kGlobalAddress};
  std::uint16_t size{0U};
  std::array<std::byte, kTpMaxMessageBytes> data{};
};

struct TpTransmitterConfig {
  std::uint8_t local_address{kNullAddress};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
  time::MonotonicDuration bam_packet_interval{50000000};
};

enum class TpTransmitterStatus : std::uint8_t {
  ok,
  no_action,
  busy,
  invalid_argument,
  invalid_state,
  invalid_time,
  queue_overflow,
  protocol_fault,
};

struct TpTransmitterCounters {
  std::uint32_t bam_messages_started{0U};
  std::uint32_t peer_messages_started{0U};
  std::uint32_t dt_packets_queued{0U};
  std::uint32_t completed_messages{0U};
  std::uint32_t aborted_messages{0U};
  std::uint32_t malformed_control_frames{0U};
  std::uint32_t timeouts{0U};
  std::uint32_t tx_queue_overflows{0U};
};

class TpTransmitter final : public transport::ICanFrameSink {
 public:
  static constexpr std::size_t kTxQueueCapacity = 16U;

  inline static constexpr time::MonotonicDuration
      kMinBamPacketInterval{50000000};
  inline static constexpr time::MonotonicDuration
      kMaxBamPacketInterval{200000000};
  inline static constexpr time::MonotonicDuration
      kResponseTimeout{200000000};
  inline static constexpr time::MonotonicDuration
      kPacketProgressTimeout{750000000};
  inline static constexpr time::MonotonicDuration
      kPeerCompletionTimeout{1250000000};
  inline static constexpr time::MonotonicDuration
      kHoldTimeout{1050000000};

  [[nodiscard]] bool configure(
      const TpTransmitterConfig& config) noexcept;
  [[nodiscard]] bool set_local_address(
      std::uint8_t address) noexcept;

  [[nodiscard]] TpTransmitterStatus submit(
      const TpTransmitMessage& message,
      const time::MonotonicClockReading& now) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] TpTransmitterStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;

  [[nodiscard]] bool bam_active() const noexcept;
  [[nodiscard]] bool peer_active() const noexcept;
  [[nodiscard]] TpTransmitterStatus status() const noexcept;
  [[nodiscard]] TpTransmitterCounters counters() const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;

 private:
  enum class SessionPhase : std::uint8_t {
    idle,
    bam_wait_packet,
    peer_wait_cts,
    peer_send_block,
    peer_wait_eom,
  };

  struct TxSession {
    bool active{false};
    bool broadcast{false};
    SessionPhase phase{SessionPhase::idle};
    std::uint8_t destination_address{kGlobalAddress};
    std::uint32_t pgn{0U};
    std::uint16_t size{0U};
    std::uint8_t packet_count{0U};
    std::uint8_t next_sequence{1U};
    std::uint8_t block_remaining{0U};
    time::MonotonicTime deadline{0};
    time::MonotonicTime next_packet_due{0};
    std::array<std::byte, kTpMaxMessageBytes> data{};
  };

  [[nodiscard]] bool valid_config(
      const TpTransmitterConfig& config) const noexcept;
  [[nodiscard]] bool valid_message(
      const TpTransmitMessage& message) const noexcept;
  [[nodiscard]] bool observe_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;

  [[nodiscard]] TpTransmitterStatus start_bam(
      const TpTransmitMessage& message,
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] TpTransmitterStatus start_peer(
      const TpTransmitMessage& message,
      const time::MonotonicClockReading& now) noexcept;
  void handle_peer_control(
      const transport::CanFrame& frame,
      const TpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] bool queue_next_dt(
      TxSession& session,
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] bool queue_timeout_abort(
      TxSession& session) noexcept;
  [[nodiscard]] bool enqueue_tx(
      const transport::CanFrame& frame) noexcept;
  void complete_session(TxSession& session) noexcept;
  void reset_session(TxSession& session) noexcept;
  void latch_fault() noexcept;

  TpTransmitterConfig config_{};
  TxSession bam_{};
  TxSession peer_{};
  std::array<transport::CanFrame, kTxQueueCapacity> tx_queue_{};
  std::size_t tx_head_{0U};
  std::size_t tx_tail_{0U};
  std::size_t tx_count_{0U};
  time::MonotonicTime last_observed_time_{0};
  bool has_last_observed_time_{false};
  bool configured_{false};
  TpTransmitterStatus status_{TpTransmitterStatus::invalid_state};
  TpTransmitterCounters counters_{};
};

}  // namespace ecu::core::v2::protocol::j1939
