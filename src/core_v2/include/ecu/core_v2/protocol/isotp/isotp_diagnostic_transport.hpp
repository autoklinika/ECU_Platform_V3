#pragma once

#include "ecu/core_v2/protocol/isotp/isotp_endpoint.hpp"
#include "ecu/core_v2/transport/i_diagnostic_transport.hpp"

#include <cstddef>

namespace ecu::core::v2::protocol::isotp {

class IsoTpDiagnosticTransport final
    : public transport::IDiagnosticTransport {
 public:
  IsoTpDiagnosticTransport(
      IsoTpEndpoint& endpoint,
      transport::CanBusRuntime& runtime) noexcept
      : endpoint_(endpoint),
        runtime_(runtime) {}

  [[nodiscard]] bool valid() const noexcept override {
    return endpoint_.valid();
  }

  [[nodiscard]] transport::DiagnosticTransportStatus start_send(
      const std::byte* const payload,
      const std::size_t length) noexcept override {
    return map_status(endpoint_.start_send(payload, length));
  }

  [[nodiscard]] transport::DiagnosticTransportStatus service(
      const time::MonotonicClockReading& now) noexcept override {
    return map_status(endpoint_.service(runtime_, now));
  }

  [[nodiscard]] bool tx_busy() const noexcept override {
    return endpoint_.tx_busy();
  }

  [[nodiscard]] transport::DiagnosticTransportStatus last_tx_status()
      const noexcept override {
    return map_status(endpoint_.last_tx_status());
  }

  [[nodiscard]] time::MonotonicClockReading
  tx_completion_timestamp() const noexcept override {
    return endpoint_.tx_completion_timestamp();
  }

  [[nodiscard]] bool has_received() const noexcept override {
    return endpoint_.has_received();
  }

  [[nodiscard]] std::size_t received_size() const noexcept override {
    return endpoint_.peek_received_length();
  }

  [[nodiscard]] transport::DiagnosticTransportStatus take_received(
      std::byte* const destination,
      const std::size_t capacity,
      std::size_t& length,
      time::MonotonicClockReading& completion_timestamp) noexcept override {
    length = 0U;
    completion_timestamp = {};
    if (destination == nullptr || capacity == 0U) {
      return transport::DiagnosticTransportStatus::invalid_argument;
    }
    if (!endpoint_.has_received()) {
      return transport::DiagnosticTransportStatus::idle;
    }
    const auto expected = endpoint_.peek_received_length();
    if (expected == 0U) {
      return transport::DiagnosticTransportStatus::protocol_error;
    }
    if (expected > capacity) {
      return transport::DiagnosticTransportStatus::payload_too_large;
    }

    const auto received = endpoint_.take_received();
    if (received.status != IsoTpStatus::ok ||
        received.length != expected) {
      return map_status(received.status);
    }

    for (std::size_t index = 0U; index < received.length; ++index) {
      destination[index] = received.payload[index];
    }
    length = received.length;
    completion_timestamp = received.completion_timestamp;
    return transport::DiagnosticTransportStatus::ok;
  }

  void reset() noexcept override {
    endpoint_.reset();
  }

 private:
  [[nodiscard]] static transport::DiagnosticTransportStatus map_status(
      const IsoTpStatus status) noexcept {
    using D = transport::DiagnosticTransportStatus;
    switch (status) {
      case IsoTpStatus::ok:
        return D::ok;
      case IsoTpStatus::in_progress:
        return D::in_progress;
      case IsoTpStatus::idle:
        return D::idle;
      case IsoTpStatus::busy:
        return D::busy;
      case IsoTpStatus::would_block:
        return D::would_block;
      case IsoTpStatus::invalid_argument:
        return D::invalid_argument;
      case IsoTpStatus::payload_too_large:
        return D::payload_too_large;
      case IsoTpStatus::timeout:
        return D::timeout;
      case IsoTpStatus::sequence_error:
        return D::sequence_error;
      case IsoTpStatus::flow_control_overflow:
        return D::flow_control_overflow;
      case IsoTpStatus::protocol_error:
        return D::protocol_error;
      case IsoTpStatus::transport_error:
        return D::transport_error;
      case IsoTpStatus::bus_off:
        return D::bus_off;
      case IsoTpStatus::clock_fault:
        return D::clock_fault;
      case IsoTpStatus::queue_overflow:
        return D::queue_overflow;
    }
    return D::transport_error;
  }

  IsoTpEndpoint& endpoint_;
  transport::CanBusRuntime& runtime_;
};

}  // namespace ecu::core::v2::protocol::isotp
