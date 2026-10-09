#pragma once

#include "ecu/core_v2/protocol/j1939/j1939_identifier.hpp"
#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::j1939 {

inline constexpr std::uint32_t kFdTpCmPgn = 0x4D00U;
inline constexpr std::uint32_t kFdTpDtPgn = 0x4E00U;
inline constexpr std::size_t kFdTpCmPayloadBytes = 12U;
inline constexpr std::size_t kFdTpDtHeaderBytes = 4U;
inline constexpr std::size_t kFdTpSegmentPayloadBytes = 60U;
inline constexpr std::uint32_t kFdTpMaxMessageBytes = 0xFFFFFFU;
inline constexpr std::uint32_t kFdTpMaxBamMessageBytes = 15300U;
inline constexpr std::uint32_t kFdTpMaxBamSegments = 255U;
inline constexpr std::uint8_t kFdTpNoAssuranceDataType = 0U;
inline constexpr std::uint8_t kFdTpNoAssuranceDtfi = 0U;

enum class FdTpControl : std::uint8_t {
  rts = 0U,
  cts = 1U,
  end_of_message_status = 2U,
  end_of_message_ack = 3U,
  bam = 4U,
  abort = 15U,
};

enum class FdTpAbortReason : std::uint8_t {
  busy = 1U,
  resources = 2U,
  timeout = 3U,
  cts_while_data_transfer = 4U,
};

struct FdTpSessionKey {
  std::uint8_t session_number{0U};
  std::uint8_t source_address{kNullAddress};
  std::uint8_t destination_address{kNullAddress};
};

[[nodiscard]] constexpr bool operator==(
    const FdTpSessionKey& lhs,
    const FdTpSessionKey& rhs) noexcept {
  return lhs.session_number == rhs.session_number &&
         lhs.source_address == rhs.source_address &&
         lhs.destination_address == rhs.destination_address;
}

[[nodiscard]] constexpr bool operator!=(
    const FdTpSessionKey& lhs,
    const FdTpSessionKey& rhs) noexcept {
  return !(lhs == rhs);
}

struct FdTpCmFrame {
  FdTpControl control{FdTpControl::abort};
  FdTpSessionKey key{};
  std::uint8_t priority{7U};
  std::uint32_t message_size{0U};
  std::uint32_t segment_number{0U};
  std::uint8_t parameter7{0U};
  std::uint8_t parameter8{0U};
  std::uint32_t transported_pgn{0U};
};

struct FdTpDtFrame {
  FdTpSessionKey key{};
  std::uint8_t dtfi{kFdTpNoAssuranceDtfi};
  std::uint32_t segment_number{0U};
  std::uint8_t wire_data_bytes{0U};
  std::array<std::byte, kFdTpSegmentPayloadBytes> data{};
};

[[nodiscard]] constexpr std::uint32_t fd_tp_segment_count(
    const std::uint32_t message_size) noexcept {
  return
      (message_size +
       static_cast<std::uint32_t>(
           kFdTpSegmentPayloadBytes - 1U)) /
      static_cast<std::uint32_t>(
          kFdTpSegmentPayloadBytes);
}

[[nodiscard]] bool decode_fd_tp_cm(
    const transport::CanFrame& frame,
    FdTpCmFrame& value) noexcept;

[[nodiscard]] bool decode_fd_tp_dt(
    const transport::CanFrame& frame,
    FdTpDtFrame& value) noexcept;

[[nodiscard]] bool build_fd_tp_rts(
    std::uint8_t priority,
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t session_number,
    std::uint32_t message_size,
    std::uint8_t max_segments_per_cts,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_fd_tp_bam(
    std::uint8_t priority,
    std::uint8_t source_address,
    std::uint8_t session_number,
    std::uint32_t message_size,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_fd_tp_cts(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t session_number,
    std::uint8_t segments_allowed,
    std::uint32_t next_segment,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_fd_tp_end_of_message_status(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t session_number,
    std::uint32_t message_size,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_fd_tp_end_of_message_ack(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t session_number,
    std::uint32_t message_size,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_fd_tp_abort(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t session_number,
    FdTpAbortReason reason,
    std::uint32_t transported_pgn,
    transport::CanFrame& frame) noexcept;

[[nodiscard]] bool build_fd_tp_dt(
    std::uint8_t source_address,
    std::uint8_t destination_address,
    std::uint8_t session_number,
    std::uint32_t segment_number,
    const std::array<std::byte, kFdTpSegmentPayloadBytes>& data,
    std::uint8_t valid_bytes,
    transport::CanFrame& frame) noexcept;

class IFdTpReceiveSink {
 public:
  [[nodiscard]] virtual bool begin(
      const FdTpSessionKey& key,
      std::uint32_t pgn,
      std::uint32_t total_size,
      bool broadcast) noexcept = 0;

  [[nodiscard]] virtual bool write(
      const FdTpSessionKey& key,
      std::uint32_t byte_offset,
      const std::array<std::byte, kFdTpSegmentPayloadBytes>& data,
      std::uint8_t valid_bytes) noexcept = 0;

  [[nodiscard]] virtual bool commit(
      const FdTpSessionKey& key) noexcept = 0;

  virtual void abort(
      const FdTpSessionKey& key) noexcept = 0;

 protected:
  ~IFdTpReceiveSink() = default;
};

struct FdTpReceiverConfig {
  std::uint8_t local_address{kNullAddress};
  std::uint8_t max_segments_per_cts{16U};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
};

enum class FdTpReceiverStatus : std::uint8_t {
  ok,
  no_action,
  invalid_state,
  invalid_time,
  queue_overflow,
  protocol_fault,
};

struct FdTpReceiverCounters {
  std::uint32_t bam_sessions_started{0U};
  std::uint32_t peer_sessions_started{0U};
  std::uint32_t completed_messages{0U};
  std::uint32_t cm_frames{0U};
  std::uint32_t dt_frames{0U};
  std::uint32_t malformed_frames{0U};
  std::uint32_t rejected_sessions{0U};
  std::uint32_t sink_failures{0U};
  std::uint32_t remote_aborts{0U};
  std::uint32_t timeouts{0U};
  std::uint32_t tx_queue_overflows{0U};
};

class FdTpReceiver final : public transport::ICanFrameSink {
 public:
  static constexpr std::size_t kBamSessionCapacity = 4U;
  static constexpr std::size_t kPeerSessionCapacity = 8U;
  static constexpr std::size_t kTxQueueCapacity = 32U;

  inline static constexpr time::MonotonicDuration
      kSegmentTimeout{750000000};
  inline static constexpr time::MonotonicDuration
      kCtsDataTimeout{1250000000};

  [[nodiscard]] bool configure(
      const FdTpReceiverConfig& config,
      IFdTpReceiveSink& sink) noexcept;

  [[nodiscard]] bool set_local_address(
      std::uint8_t address) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] FdTpReceiverStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;

  [[nodiscard]] FdTpReceiverStatus status() const noexcept;
  [[nodiscard]] FdTpReceiverCounters counters() const noexcept;
  [[nodiscard]] std::size_t active_session_count() const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;

 private:
  struct Session {
    bool active{false};
    bool broadcast{false};
    bool waiting_eom_status{false};
    FdTpSessionKey key{};
    std::uint32_t pgn{0U};
    std::uint32_t total_size{0U};
    std::uint32_t total_segments{0U};
    std::uint32_t next_segment{1U};
    std::uint32_t bytes_received{0U};
    std::uint8_t granted_remaining{0U};
    time::MonotonicTime deadline{0};
  };

  [[nodiscard]] bool valid_config(
      const FdTpReceiverConfig& config) const noexcept;
  [[nodiscard]] bool observe_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;

  [[nodiscard]] Session* find_session(
      const FdTpSessionKey& key) noexcept;
  [[nodiscard]] Session* allocate_session(
      bool broadcast) noexcept;

  void handle_cm(
      const FdTpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void handle_dt(
      const FdTpDtFrame& dt,
      const time::MonotonicClockReading& timestamp) noexcept;
  void start_bam(
      const FdTpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void start_peer(
      const FdTpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void accept_eom_status(
      const FdTpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void accept_abort(
      const FdTpCmFrame& cm) noexcept;
  void accept_dt(
      Session& session,
      const FdTpDtFrame& dt,
      const time::MonotonicClockReading& timestamp) noexcept;

  [[nodiscard]] bool queue_cts(
      const Session& session,
      std::uint8_t segments_allowed,
      std::uint32_t next_segment) noexcept;
  [[nodiscard]] bool queue_eom_ack(
      const Session& session) noexcept;
  [[nodiscard]] bool queue_abort(
      const FdTpSessionKey& key,
      FdTpAbortReason reason,
      std::uint32_t pgn) noexcept;
  [[nodiscard]] bool enqueue_tx(
      const transport::CanFrame& frame) noexcept;

  void release_session(
      Session& session,
      bool notify_sink) noexcept;
  void complete_session(
      Session& session) noexcept;
  void latch_fault() noexcept;

  FdTpReceiverConfig config_{};
  IFdTpReceiveSink* sink_{nullptr};
  std::array<Session, kBamSessionCapacity> bam_sessions_{};
  std::array<Session, kPeerSessionCapacity> peer_sessions_{};
  std::array<transport::CanFrame, kTxQueueCapacity> tx_queue_{};
  std::size_t tx_head_{0U};
  std::size_t tx_tail_{0U};
  std::size_t tx_count_{0U};
  time::MonotonicTime last_observed_time_{0};
  bool has_last_observed_time_{false};
  bool configured_{false};
  FdTpReceiverStatus status_{FdTpReceiverStatus::invalid_state};
  FdTpReceiverCounters counters_{};
};

class IFdTpTransmitSource {
 public:
  [[nodiscard]] virtual bool read(
      std::uint32_t byte_offset,
      std::array<std::byte, kFdTpSegmentPayloadBytes>& data,
      std::uint8_t& valid_bytes) noexcept = 0;

 protected:
  ~IFdTpTransmitSource() = default;
};

struct FdTpTransmitRequest {
  std::uint32_t pgn{0U};
  std::uint8_t destination_address{kNullAddress};
  std::uint32_t total_size{0U};
  std::uint8_t priority{7U};
};

struct FdTpTransmitterConfig {
  std::uint8_t local_address{kNullAddress};
  std::uint8_t max_segments_per_cts{16U};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
};

enum class FdTpTransmitterStatus : std::uint8_t {
  ok,
  no_action,
  busy,
  invalid_argument,
  invalid_state,
  invalid_time,
  queue_overflow,
  protocol_fault,
};

struct FdTpTransmitterCounters {
  std::uint32_t bam_sessions_started{0U};
  std::uint32_t peer_sessions_started{0U};
  std::uint32_t completed_messages{0U};
  std::uint32_t dt_frames{0U};
  std::uint32_t eom_status_frames{0U};
  std::uint32_t source_failures{0U};
  std::uint32_t malformed_control_frames{0U};
  std::uint32_t remote_aborts{0U};
  std::uint32_t timeouts{0U};
  std::uint32_t tx_queue_overflows{0U};
};

class FdTpTransmitter final : public transport::ICanFrameSink {
 public:
  static constexpr std::size_t kBamSessionCapacity = 4U;
  static constexpr std::size_t kPeerSessionCapacity = 8U;
  static constexpr std::size_t kTxQueueCapacity = 32U;

  inline static constexpr time::MonotonicDuration
      kResponseTimeout{1250000000};
  inline static constexpr time::MonotonicDuration
      kHoldTimeout{1050000000};
  inline static constexpr time::MonotonicDuration
      kEomAckTimeout{3000000000};
  inline static constexpr time::MonotonicDuration
      kBamSegmentInterval{10000000};

  [[nodiscard]] bool configure(
      const FdTpTransmitterConfig& config) noexcept;

  [[nodiscard]] bool set_local_address(
      std::uint8_t address) noexcept;

  [[nodiscard]] FdTpTransmitterStatus submit(
      const FdTpTransmitRequest& request,
      IFdTpTransmitSource& source,
      const time::MonotonicClockReading& now) noexcept;

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override;

  [[nodiscard]] FdTpTransmitterStatus service_time(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool try_take_tx(
      transport::CanFrame& frame) noexcept;

  [[nodiscard]] FdTpTransmitterStatus status() const noexcept;
  [[nodiscard]] FdTpTransmitterCounters counters() const noexcept;
  [[nodiscard]] std::size_t active_session_count() const noexcept;
  [[nodiscard]] std::size_t pending_tx_count() const noexcept;

 private:
  enum class Phase : std::uint8_t {
    idle,
    send_data,
    wait_cts,
    send_eom_status,
    wait_eom_ack,
  };

  struct Session {
    bool active{false};
    bool broadcast{false};
    std::uint8_t session_number{0U};
    Phase phase{Phase::idle};
    FdTpTransmitRequest request{};
    IFdTpTransmitSource* source{nullptr};
    std::uint32_t total_segments{0U};
    std::uint32_t next_segment{1U};
    std::uint8_t grant_remaining{0U};
    time::MonotonicTime deadline{0};
    time::MonotonicTime next_action{0};
  };

  [[nodiscard]] bool valid_config(
      const FdTpTransmitterConfig& config) const noexcept;
  [[nodiscard]] bool valid_request(
      const FdTpTransmitRequest& request) const noexcept;
  [[nodiscard]] bool observe_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;

  [[nodiscard]] Session* allocate_session(
      bool broadcast) noexcept;
  [[nodiscard]] Session* find_peer_session(
      const FdTpCmFrame& cm) noexcept;

  void handle_control(
      const FdTpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void accept_cts(
      Session& session,
      const FdTpCmFrame& cm,
      const time::MonotonicClockReading& timestamp) noexcept;
  void accept_eom_ack(
      Session& session,
      const FdTpCmFrame& cm) noexcept;
  void accept_abort(
      Session& session) noexcept;

  [[nodiscard]] bool queue_next_dt(
      Session& session,
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] bool queue_eom_status(
      Session& session,
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] bool queue_abort(
      Session& session,
      FdTpAbortReason reason) noexcept;
  [[nodiscard]] bool enqueue_tx(
      const transport::CanFrame& frame) noexcept;

  void reset_session(Session& session) noexcept;
  void latch_fault() noexcept;

  FdTpTransmitterConfig config_{};
  std::array<Session, kBamSessionCapacity> bam_sessions_{};
  std::array<Session, kPeerSessionCapacity> peer_sessions_{};
  std::array<transport::CanFrame, kTxQueueCapacity> tx_queue_{};
  std::size_t tx_head_{0U};
  std::size_t tx_tail_{0U};
  std::size_t tx_count_{0U};
  std::size_t service_cursor_{0U};
  time::MonotonicTime last_observed_time_{0};
  bool has_last_observed_time_{false};
  bool configured_{false};
  FdTpTransmitterStatus status_{
      FdTpTransmitterStatus::invalid_state};
  FdTpTransmitterCounters counters_{};
};

}  // namespace ecu::core::v2::protocol::j1939
