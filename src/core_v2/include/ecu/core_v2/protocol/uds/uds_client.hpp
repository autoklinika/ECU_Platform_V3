#pragma once

#include "ecu/core_v2/protocol/uds/uds_types.hpp"
#include "ecu/core_v2/transport/i_diagnostic_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::protocol::uds {

class UdsClient final {
 public:
  UdsClient(
      transport::IDiagnosticTransport& transport,
      UdsClientConfig config) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool busy() const noexcept;
  [[nodiscard]] bool faulted() const noexcept;

  [[nodiscard]] UdsStatus start_request(
      const std::byte* payload,
      std::size_t length) noexcept;
  [[nodiscard]] UdsStatus start_request(
      const UdsRequest& request) noexcept;
  [[nodiscard]] UdsStatus start_request(
      UdsRequestView request) noexcept;

  [[nodiscard]] UdsStatus service(
      const time::MonotonicClockReading& now) noexcept;

  [[nodiscard]] bool has_response() const noexcept;
  [[nodiscard]] UdsResponse take_response() noexcept;

  [[nodiscard]] UdsTiming timing() const noexcept;
  void set_timing(UdsTiming timing) noexcept;
  void reset() noexcept;

 private:
  enum class State : std::uint8_t {
    idle,
    sending,
    waiting_p2,
    waiting_p2_star,
    complete,
  };

  [[nodiscard]] UdsStatus handle_transport_response(
      const std::byte* payload,
      std::size_t length,
      const time::MonotonicClockReading& completion_timestamp) noexcept;

  [[nodiscard]] UdsStatus map_transport_status(
      transport::DiagnosticTransportStatus status) const noexcept;
  [[nodiscard]] UdsTransportFailure map_transport_failure(
      transport::DiagnosticTransportStatus status) const noexcept;

  [[nodiscard]] bool observe_service_time(
      const time::MonotonicClockReading& reading) noexcept;
  [[nodiscard]] bool valid_transport_timestamp(
      const time::MonotonicClockReading& reading) const noexcept;
  [[nodiscard]] bool observe_protocol_timestamp(
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

  void complete_with_status(
      UdsStatus status,
      UdsTransportFailure transport_failure) noexcept;
  void latch_clock_fault() noexcept;

  transport::IDiagnosticTransport& transport_;
  UdsClientConfig config_{};
  bool valid_{false};
  bool faulted_{false};

  State state_{State::idle};
  std::uint8_t request_sid_{0U};
  time::MonotonicTime deadline_{0};
  time::MonotonicTime last_service_time_{0};
  bool has_last_service_time_{false};
  time::MonotonicTime last_protocol_time_{0};
  bool has_last_protocol_time_{false};

  UdsResponse response_{};
  bool response_ready_{false};
  std::array<std::byte, kMaxUdsPayloadSize> transport_rx_buffer_{};
};

}  // namespace ecu::core::v2::protocol::uds
