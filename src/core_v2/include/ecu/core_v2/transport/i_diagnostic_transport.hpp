#pragma once

#include "ecu/core_v2/time/monotonic_clock.hpp"

#include <cstddef>
#include <cstdint>

namespace ecu::core::v2::transport {

enum class DiagnosticTransportStatus : std::uint8_t {
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

class IDiagnosticTransport {
 public:
  [[nodiscard]] virtual bool valid() const noexcept = 0;

  [[nodiscard]] virtual DiagnosticTransportStatus start_send(
      const std::byte* payload,
      std::size_t length) noexcept = 0;

  // Advances transport-owned TX/control state only. The owning runtime remains
  // responsible for receiving/dispatching link frames before this call.
  [[nodiscard]] virtual DiagnosticTransportStatus service(
      const time::MonotonicClockReading& now) noexcept = 0;

  [[nodiscard]] virtual bool tx_busy() const noexcept = 0;
  [[nodiscard]] virtual DiagnosticTransportStatus last_tx_status()
      const noexcept = 0;
  [[nodiscard]] virtual time::MonotonicClockReading
  tx_completion_timestamp() const noexcept = 0;

  [[nodiscard]] virtual bool has_received() const noexcept = 0;
  [[nodiscard]] virtual std::size_t received_size() const noexcept = 0;

  [[nodiscard]] virtual DiagnosticTransportStatus take_received(
      std::byte* destination,
      std::size_t capacity,
      std::size_t& length,
      time::MonotonicClockReading& completion_timestamp) noexcept = 0;

  virtual void reset() noexcept = 0;

 protected:
  ~IDiagnosticTransport() = default;
};

}  // namespace ecu::core::v2::transport
