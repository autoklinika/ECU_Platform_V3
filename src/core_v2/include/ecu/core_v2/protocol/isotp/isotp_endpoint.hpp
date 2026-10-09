#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::isotp {

inline constexpr std::size_t kMaxPayloadSize = 4095U;

enum class IsoTpStatus : std::uint8_t {
  ok,
  in_progress,
  idle,
  busy,
  would_block,
  invalid_argument,
  payload_too_large,
  timeout,
  sequence_error,
  flow_control_overflow,
  protocol_error,
  transport_error,
  bus_off,
  clock_fault,
  queue_overflow,
};

struct IsoTpAddress {
  std::uint32_t tx_id{0U};
  std::uint32_t rx_id{0U};
  transport::CanIdentifierFormat identifier_format{
      transport::CanIdentifierFormat::standard_11_bit};
};

struct IsoTpConfig {
  transport::CanFrameFormat frame_format{
      transport::CanFrameFormat::classic};
  std::uint8_t tx_data_length{8U};
  bool bit_rate_switch{false};

  std::uint8_t rx_block_size{0U};
  std::uint8_t rx_stmin{0U};
  std::uint8_t max_wait_frames{3U};

  time::MonotonicDuration flow_control_timeout{1000000000LL};
  time::MonotonicDuration consecutive_frame_timeout{1000000000LL};
  time::MonotonicClockDomainId timestamp_domain{};
  time::MonotonicDuration max_timestamp_uncertainty{0};
};

struct IsoTpReceiveResult {
  IsoTpStatus status{IsoTpStatus::idle};
  std::size_t length{0U};
  time::MonotonicClockReading completion_timestamp{};
  std::array<std::byte, kMaxPayloadSize> payload{};
};

[[nodiscard]] bool is_valid_isotp_address(
    const IsoTpAddress& address) noexcept;

[[nodiscard]] bool is_valid_isotp_config(
    const IsoTpConfig& config) noexcept;

[[nodiscard]] time::MonotonicDuration decode_stmin(
    std::uint8_t encoded,
    bool& valid) noexcept;

class IsoTpEndpoint final {
 public:
  IsoTpEndpoint(
      IsoTpAddress address,
      IsoTpConfig config) noexcept;

  [[nodiscard]] bool valid() const noexcept;

  [[nodiscard]] IsoTpStatus start_send(
      const std::byte* payload,
      std::size_t length) noexcept;

  // Called outside CanBusRuntime::poll(). At most one CAN TX attempt is made
  // per call. This is the only endpoint path that invokes runtime.send().
  [[nodiscard]] IsoTpStatus service(
      transport::CanBusRuntime& runtime,
      const time::MonotonicClockReading& now) noexcept;

  // RX callback only mutates bounded local state and may queue one deferred
  // Flow Control frame. It never calls runtime.send() or runtime.poll().
  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept;

  [[nodiscard]] bool tx_busy() const noexcept;
  [[nodiscard]] IsoTpStatus last_tx_status() const noexcept;
  [[nodiscard]] time::MonotonicClockReading
  tx_completion_timestamp() const noexcept;
  [[nodiscard]] IsoTpStatus last_rx_status() const noexcept;

  [[nodiscard]] bool has_received() const noexcept;
  [[nodiscard]] std::size_t peek_received_length() const noexcept;
  [[nodiscard]] IsoTpReceiveResult take_received() noexcept;

  [[nodiscard]] bool pending_control() const noexcept;
  void reset() noexcept;

 private:
  enum class TxState : std::uint8_t {
    idle,
    single_pending,
    first_pending,
    waiting_flow_control,
    sending_consecutive,
  };

  [[nodiscard]] IsoTpStatus process_incoming(
      const transport::ReceivedCanFrame& received) noexcept;
  [[nodiscard]] IsoTpStatus process_flow_control(
      const transport::CanFrame& frame,
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] IsoTpStatus process_single_frame(
      const transport::CanFrame& frame,
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] IsoTpStatus process_first_frame(
      const transport::CanFrame& frame,
      const time::MonotonicClockReading& timestamp) noexcept;
  [[nodiscard]] IsoTpStatus process_consecutive_frame(
      const transport::CanFrame& frame,
      const time::MonotonicClockReading& timestamp) noexcept;

  [[nodiscard]] IsoTpStatus service_tx(
      transport::CanBusRuntime& runtime,
      const time::MonotonicClockReading& now) noexcept;
  [[nodiscard]] IsoTpStatus send_pending_control(
      transport::CanBusRuntime& runtime) noexcept;
  [[nodiscard]] IsoTpStatus queue_flow_control(
      std::uint8_t flow_status) noexcept;

  [[nodiscard]] transport::CanFrame make_base_tx_frame() const noexcept;
  [[nodiscard]] std::uint8_t choose_wire_length(
      std::uint8_t used_bytes) const noexcept;
  [[nodiscard]] std::size_t single_frame_capacity() const noexcept;
  [[nodiscard]] IsoTpStatus map_can_status(
      transport::CanStatus status) const noexcept;

  [[nodiscard]] bool observe_service_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool observe_receive_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool set_deadline_after(
      const time::MonotonicClockReading& reading,
      time::MonotonicDuration delay,
      time::MonotonicTime& deadline) const noexcept;
  [[nodiscard]] time::MonotonicTime lower_bound(
      const time::MonotonicClockReading& reading) const noexcept;
  [[nodiscard]] bool deadline_reached(
      const time::MonotonicClockReading& reading,
      time::MonotonicTime deadline) const noexcept;

  void fail_tx(IsoTpStatus status) noexcept;
  void reset_rx_transfer() noexcept;
  void latch_clock_fault() noexcept;
  void post_event(IsoTpStatus status) noexcept;

  IsoTpAddress address_{};
  IsoTpConfig config_{};
  bool valid_{false};
  bool faulted_{false};

  TxState tx_state_{TxState::idle};
  IsoTpStatus last_tx_status_{IsoTpStatus::idle};
  std::array<std::byte, kMaxPayloadSize> tx_payload_{};
  std::size_t tx_length_{0U};
  std::size_t tx_offset_{0U};
  std::uint8_t tx_sequence_{1U};
  std::uint8_t tx_block_size_{0U};
  std::uint8_t tx_block_sent_{0U};
  std::uint8_t tx_wait_frames_{0U};
  time::MonotonicDuration tx_stmin_{0};
  time::MonotonicTime tx_next_send_{0};
  time::MonotonicTime tx_deadline_{0};
  time::MonotonicClockReading tx_completion_timestamp_{};

  bool rx_active_{false};
  bool rx_complete_{false};
  IsoTpStatus last_rx_status_{IsoTpStatus::idle};
  std::array<std::byte, kMaxPayloadSize> rx_payload_{};
  std::size_t rx_length_{0U};
  std::size_t rx_offset_{0U};
  std::uint8_t rx_expected_sequence_{1U};
  std::uint8_t rx_block_received_{0U};
  time::MonotonicTime rx_deadline_{0};
  time::MonotonicClockReading rx_completion_timestamp_{};

  bool pending_control_{false};
  transport::CanFrame control_frame_{};
  IsoTpStatus pending_event_{IsoTpStatus::idle};

  time::MonotonicTime last_service_time_{0};
  time::MonotonicTime last_receive_time_{0};
  bool has_last_service_time_{false};
  bool has_last_receive_time_{false};
};

// Integration adapter intentionally stays header-only. Keeping the protocol
// state machine itself non-polymorphic prevents compiler-specific pure-virtual
// runtime dependencies from leaking into the Core V2 static library.
class IsoTpCanFrameSinkAdapter final : public transport::ICanFrameSink {
 public:
  explicit IsoTpCanFrameSinkAdapter(
      IsoTpEndpoint& endpoint) noexcept
      : endpoint_(endpoint) {}

  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override {
    endpoint_.on_can_frame(frame);
  }

 private:
  IsoTpEndpoint& endpoint_;
};

}  // namespace ecu::core::v2::protocol::isotp
