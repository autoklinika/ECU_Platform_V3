#pragma once

#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::isobus {

inline constexpr std::uint32_t kEtpCmPgn = 0xC800U;
inline constexpr std::uint32_t kEtpDtPgn = 0xC700U;
inline constexpr std::uint32_t kEtpMinMessageBytes = 1786U;
inline constexpr std::uint32_t kEtpMaxPacketCount = 0xFFFFFFU;
inline constexpr std::uint32_t kEtpMaxMessageBytes =
    kEtpMaxPacketCount * 7U;
inline constexpr std::size_t kEtpPacketPayloadBytes = 7U;

enum class EtpControl : std::uint8_t {
  rts = 0x14U,
  cts = 0x15U,
  dpo = 0x16U,
  eoma = 0x17U,
  abort = 0xFFU,
};

enum class EtpAbortReason : std::uint8_t {
  already_in_session = 1U,
  resources = 2U,
  timeout = 3U,
};

struct EtpCmFrame {
  EtpControl control{EtpControl::abort};
  std::uint8_t source_address{0U};
  std::uint8_t destination_address{0U};
  std::uint32_t message_size{0U};
  std::uint8_t packets_allowed{0U};
  std::uint32_t next_packet{0U};
  std::uint8_t block_count{0U};
  std::uint32_t packet_offset{0U};
  std::uint8_t abort_reason{0U};
  std::uint32_t transported_pgn{0U};
};

struct EtpDtFrame {
  std::uint8_t source_address{0U};
  std::uint8_t destination_address{0U};
  std::uint8_t sequence_number{0U};
  std::array<std::byte, kEtpPacketPayloadBytes> data{};
};

[[nodiscard]] constexpr std::uint32_t etp_packet_count(
    const std::uint32_t message_size) noexcept {
  return (message_size + 6U) / 7U;
}

[[nodiscard]] bool decode_etp_cm(
    const transport::CanFrame& frame,
    EtpCmFrame& value) noexcept;

[[nodiscard]] bool decode_etp_dt(
    const transport::CanFrame& frame,
    EtpDtFrame& value) noexcept;

[[nodiscard]] bool build_etp_rts(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint32_t message_size,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_etp_cts(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t packets_allowed,
    std::uint32_t next_packet,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_etp_dpo(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t block_count,
    std::uint32_t packet_offset,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_etp_eoma(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint32_t message_size,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_etp_abort(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    EtpAbortReason reason,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_etp_dt(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t sequence_number,
    const std::array<std::byte, kEtpPacketPayloadBytes>& data,
    transport::CanFrame& frame) noexcept;

class IEtpReceiveSink {
 public:
  [[nodiscard]] virtual bool begin(
      std::uint32_t pgn,
      std::uint8_t source_address,
      std::uint8_t destination_address,
      std::uint32_t total_size) noexcept = 0;

  [[nodiscard]] virtual bool write(
      std::uint32_t byte_offset,
      const std::array<std::byte, kEtpPacketPayloadBytes>& data,
      std::uint8_t valid_bytes) noexcept = 0;

  [[nodiscard]] virtual bool commit() noexcept = 0;
  virtual void abort() noexcept = 0;

 protected:
  ~IEtpReceiveSink() = default;
};

struct EtpReceiverConfig {
  std::uint8_t local_address{
      j1939::kNullAddress};
  std::uint8_t max_packets_per_cts{16U};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
};

enum class EtpReceiverStatus : std::uint8_t {
  ok,
  no_action,
  invalid_state,
  invalid_time,
  queue_overflow,
  protocol_fault,
};

struct EtpReceiverCounters {
  std::uint32_t sessions_started{0U};
  std::uint32_t completed_messages{0U};
  std::uint32_t cm_frames{0U};
  std::uint32_t dt_frames{0U};
  std::uint32_t malformed_frames{0U};
  std::uint32_t rejected_sessions{0U};
  std::uint32_t sink_failures{0U};
  std::uint32_t timeouts{0U};
  std::uint32_t tx_queue_overflows{0U};
};

class EtpReceiver final : public transport::ICanFrameSink {
 public:
  static constexpr std::size_t kTxQueueCapacity = 8U;

  inline static constexpr time::MonotonicDuration
      kControlTimeout{1250000000};
  inline static constexpr time::MonotonicDuration
      kPacketTimeout{750000000};

  [[nodiscard]] bool configure(
      const EtpReceiverConfig& config,
      IEtpReceiveSink& sink) noexcept;

  [[nodiscard]] bool set_local_address(
      std::uint8_t address) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] EtpReceiverStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;

  [[nodiscard]] EtpReceiverStatus status() const noexcept;
  [[nodiscard]] EtpReceiverCounters counters() const noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;

 private:
  enum class Phase : std::uint8_t {
    idle,
    wait_dpo,
    receive_block,
  };

  struct Session {
    bool active{false};
    Phase phase{Phase::idle};
    std::uint8_t source_address{0U};
    std::uint32_t pgn{0U};
    std::uint32_t total_size{0U};
    std::uint32_t total_packets{0U};
    std::uint32_t packets_received{0U};
    std::uint32_t block_offset{0U};
    std::uint8_t block_count{0U};
    std::uint8_t block_received{0U};
    std::uint8_t expected_sequence{1U};
    std::uint8_t granted_packets{0U};
    time::MonotonicTime deadline{0};
  };

  [[nodiscard]] bool valid_config(
      const EtpReceiverConfig& config) const noexcept;
  [[nodiscard]] bool observe_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;

  void handle_cm(
      const transport::CanFrame& frame,
      const EtpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void handle_dt(
      const EtpDtFrame& dt,
      const time::MonotonicClockReading& timestamp) noexcept;
  void start_session(
      const EtpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void accept_dpo(
      const EtpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void accept_dt(
      const EtpDtFrame& dt,
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] bool queue_next_cts(
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] bool queue_eoma() noexcept;
  [[nodiscard]] bool queue_abort(
      std::uint8_t destination,
      EtpAbortReason reason,
      std::uint32_t pgn) noexcept;
  [[nodiscard]] bool enqueue_tx(
      const transport::CanFrame& frame) noexcept;
  void abort_session(bool notify_sink) noexcept;
  void latch_fault(bool notify_sink) noexcept;

  EtpReceiverConfig config_{};
  IEtpReceiveSink* sink_{nullptr};
  Session session_{};
  std::array<transport::CanFrame, kTxQueueCapacity> tx_queue_{};
  std::size_t tx_head_{0U};
  std::size_t tx_tail_{0U};
  std::size_t tx_count_{0U};
  time::MonotonicTime last_observed_time_{0};
  bool has_last_observed_time_{false};
  bool configured_{false};
  EtpReceiverStatus status_{EtpReceiverStatus::invalid_state};
  EtpReceiverCounters counters_{};
};

class IEtpTransmitSource {
 public:
  [[nodiscard]] virtual bool read(
      std::uint32_t byte_offset,
      std::array<std::byte, kEtpPacketPayloadBytes>& data,
      std::uint8_t& valid_bytes) noexcept = 0;

 protected:
  ~IEtpTransmitSource() = default;
};

struct EtpTransmitRequest {
  std::uint32_t pgn{0U};
  std::uint8_t destination_address{
      j1939::kNullAddress};
  std::uint32_t total_size{0U};
};

struct EtpTransmitterConfig {
  std::uint8_t local_address{
      j1939::kNullAddress};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
};

enum class EtpTransmitterStatus : std::uint8_t {
  ok,
  no_action,
  busy,
  invalid_argument,
  invalid_state,
  invalid_time,
  queue_overflow,
  protocol_fault,
};

struct EtpTransmitterCounters {
  std::uint32_t sessions_started{0U};
  std::uint32_t completed_messages{0U};
  std::uint32_t dpo_frames{0U};
  std::uint32_t dt_frames{0U};
  std::uint32_t source_failures{0U};
  std::uint32_t malformed_control_frames{0U};
  std::uint32_t remote_aborts{0U};
  std::uint32_t timeouts{0U};
  std::uint32_t tx_queue_overflows{0U};
};

class EtpTransmitter final : public transport::ICanFrameSink {
 public:
  static constexpr std::size_t kTxQueueCapacity = 16U;

  inline static constexpr time::MonotonicDuration
      kResponseTimeout{1250000000};
  inline static constexpr time::MonotonicDuration
      kHoldTimeout{1050000000};
  inline static constexpr time::MonotonicDuration
      kProgressTimeout{750000000};

  [[nodiscard]] bool configure(
      const EtpTransmitterConfig& config) noexcept;

  [[nodiscard]] bool set_local_address(
      std::uint8_t address) noexcept;

  [[nodiscard]] EtpTransmitterStatus submit(
      const EtpTransmitRequest& request,
      IEtpTransmitSource& source,
      const time::MonotonicClockReading& now) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] EtpTransmitterStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;

  [[nodiscard]] EtpTransmitterStatus status() const noexcept;
  [[nodiscard]] EtpTransmitterCounters counters() const noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;

 private:
  enum class Phase : std::uint8_t {
    idle,
    wait_cts,
    send_block,
    wait_eoma,
  };

  struct Session {
    bool active{false};
    Phase phase{Phase::idle};
    std::uint8_t destination_address{0U};
    std::uint32_t pgn{0U};
    std::uint32_t total_size{0U};
    std::uint32_t total_packets{0U};
    std::uint32_t packet_offset{0U};
    std::uint8_t block_count{0U};
    std::uint8_t next_sequence{1U};
    std::uint8_t block_remaining{0U};
    time::MonotonicTime deadline{0};
  };

  [[nodiscard]] bool valid_config(
      const EtpTransmitterConfig& config) const noexcept;
  [[nodiscard]] bool valid_request(
      const EtpTransmitRequest& request) const noexcept;
  [[nodiscard]] bool observe_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;

  void handle_control(
      const transport::CanFrame& frame,
      const EtpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] bool start_block(
      const transport::CanFrame& frame,
      const EtpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] bool queue_next_dt(
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] bool queue_abort(
      EtpAbortReason reason) noexcept;
  [[nodiscard]] bool enqueue_tx(
      const transport::CanFrame& frame) noexcept;
  void reset_session() noexcept;
  void latch_fault() noexcept;

  EtpTransmitterConfig config_{};
  IEtpTransmitSource* source_{nullptr};
  Session session_{};
  std::array<transport::CanFrame, kTxQueueCapacity> tx_queue_{};
  std::size_t tx_head_{0U};
  std::size_t tx_tail_{0U};
  std::size_t tx_count_{0U};
  time::MonotonicTime last_observed_time_{0};
  bool has_last_observed_time_{false};
  bool configured_{false};
  EtpTransmitterStatus status_{
      EtpTransmitterStatus::invalid_state};
  EtpTransmitterCounters counters_{};
};

}  // namespace ecu::core::v2::protocol::isobus
