#include "ecu/core_v2/protocol/uds/uds_client.hpp"
#include "ecu/core_v2/protocol/uds/uds_services.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::uds;

constexpr time::MonotonicClockDomainId kDomain{77U};

[[nodiscard]] time::MonotonicClockReading reading(
    const time::MonotonicTime value,
    const time::MonotonicClockDomainId domain = kDomain,
    const time::MonotonicDuration uncertainty =
        time::MonotonicDuration{0}) noexcept {
  return {
      time::MonotonicClockStatus::ok,
      domain,
      value,
      uncertainty};
}

class FakeDiagnosticTransport final
    : public transport::IDiagnosticTransport {
 public:
  [[nodiscard]] bool valid() const noexcept override {
    return valid_;
  }

  [[nodiscard]] transport::DiagnosticTransportStatus start_send(
      const std::byte* const payload,
      const std::size_t length) noexcept override {
    if (!valid_ || payload == nullptr || length == 0U) {
      return transport::DiagnosticTransportStatus::invalid_argument;
    }
    if (length > request_.size()) {
      return transport::DiagnosticTransportStatus::payload_too_large;
    }
    if (tx_busy_) {
      return transport::DiagnosticTransportStatus::busy;
    }

    for (std::size_t index = 0U; index < length; ++index) {
      request_[index] = payload[index];
    }
    request_length_ = length;
    tx_busy_ = true;
    last_tx_status_ =
        transport::DiagnosticTransportStatus::in_progress;
    tx_completion_ = {};
    return transport::DiagnosticTransportStatus::in_progress;
  }

  [[nodiscard]] transport::DiagnosticTransportStatus service(
      const time::MonotonicClockReading&) noexcept override {
    return service_status_;
  }

  [[nodiscard]] bool tx_busy() const noexcept override {
    return tx_busy_;
  }

  [[nodiscard]] transport::DiagnosticTransportStatus last_tx_status()
      const noexcept override {
    return last_tx_status_;
  }

  [[nodiscard]] time::MonotonicClockReading
  tx_completion_timestamp() const noexcept override {
    return tx_completion_;
  }

  [[nodiscard]] bool has_received() const noexcept override {
    return rx_ready_;
  }

  [[nodiscard]] std::size_t received_size() const noexcept override {
    return rx_ready_ ? rx_length_ : 0U;
  }

  [[nodiscard]] transport::DiagnosticTransportStatus take_received(
      std::byte* const destination,
      const std::size_t capacity,
      std::size_t& length,
      time::MonotonicClockReading& completion_timestamp) noexcept override {
    length = 0U;
    completion_timestamp = {};
    if (!rx_ready_) {
      return transport::DiagnosticTransportStatus::idle;
    }
    if (destination == nullptr || capacity < rx_length_) {
      return transport::DiagnosticTransportStatus::payload_too_large;
    }

    for (std::size_t index = 0U; index < rx_length_; ++index) {
      destination[index] = response_[index];
    }
    length = rx_length_;
    completion_timestamp = rx_completion_;
    rx_ready_ = false;
    rx_length_ = 0U;
    rx_completion_ = {};
    return transport::DiagnosticTransportStatus::ok;
  }

  void reset() noexcept override {
    ++reset_count_;
    tx_busy_ = false;
    last_tx_status_ =
        transport::DiagnosticTransportStatus::idle;
    tx_completion_ = {};
    rx_ready_ = false;
    rx_length_ = 0U;
    rx_completion_ = {};
    service_status_ =
        transport::DiagnosticTransportStatus::ok;
  }

  void complete_tx(
      const time::MonotonicClockReading timestamp) noexcept {
    tx_busy_ = false;
    last_tx_status_ =
        transport::DiagnosticTransportStatus::ok;
    tx_completion_ = timestamp;
  }

  void fail_tx(
      const transport::DiagnosticTransportStatus status) noexcept {
    tx_busy_ = false;
    last_tx_status_ = status;
  }

  void set_service_status(
      const transport::DiagnosticTransportStatus status) noexcept {
    service_status_ = status;
  }

  void queue_response(
      const std::initializer_list<std::uint8_t> bytes,
      const time::MonotonicClockReading timestamp) noexcept {
    rx_length_ = 0U;
    for (const auto value : bytes) {
      response_[rx_length_] = static_cast<std::byte>(value);
      ++rx_length_;
    }
    rx_completion_ = timestamp;
    rx_ready_ = true;
  }

  [[nodiscard]] std::size_t request_length() const noexcept {
    return request_length_;
  }

  [[nodiscard]] std::uint8_t request_byte(
      const std::size_t index) const noexcept {
    return std::to_integer<std::uint8_t>(request_[index]);
  }

  [[nodiscard]] std::uint32_t reset_count() const noexcept {
    return reset_count_;
  }

 private:
  bool valid_{true};
  bool tx_busy_{false};
  transport::DiagnosticTransportStatus last_tx_status_{
      transport::DiagnosticTransportStatus::idle};
  time::MonotonicClockReading tx_completion_{};
  transport::DiagnosticTransportStatus service_status_{
      transport::DiagnosticTransportStatus::ok};

  std::array<std::byte, kMaxUdsPayloadSize> request_{};
  std::size_t request_length_{0U};

  bool rx_ready_{false};
  std::array<std::byte, kMaxUdsPayloadSize> response_{};
  std::size_t rx_length_{0U};
  time::MonotonicClockReading rx_completion_{};
  std::uint32_t reset_count_{0U};
};

[[nodiscard]] UdsClientConfig config() noexcept {
  UdsClientConfig result{};
  result.timing.p2 = std::chrono::milliseconds{5};
  result.timing.p2_star = std::chrono::milliseconds{50};
  result.timestamp_domain = kDomain;
  result.max_timestamp_uncertainty =
      std::chrono::microseconds{100};
  return result;
}

int require(
    const bool condition,
    const char* const message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;

  {
    auto invalid = config();
    invalid.timestamp_domain = {};
    FakeDiagnosticTransport transport;
    UdsClient client{transport, invalid};
    failures += require(
        !client.valid(),
        "UDS requires explicit clock domain");

    std::uint8_t response_sid = 0U;
    failures += require(
        positive_response_sid(0x22U, response_sid) &&
            response_sid == 0x62U,
        "positive response SID mapping");
    failures += require(
        !positive_response_sid(0xC0U, response_sid),
        "overflowing positive response SID rejected");
  }

  {
    const auto session =
        make_diagnostic_session_control(0x03U);
    const auto did =
        make_read_data_by_identifier(0xF190U);
    const auto dtc =
        make_read_dtc_information_by_status_mask(0xFFU);
    const auto tester = make_tester_present();

    failures += require(
        session.length == 2U &&
            std::to_integer<std::uint8_t>(
                session.payload[0U]) == 0x10U &&
            std::to_integer<std::uint8_t>(
                session.payload[1U]) == 0x03U,
        "DiagnosticSessionControl builder");
    failures += require(
        make_diagnostic_session_control(0U).length == 0U,
        "invalid diagnostic session rejected");
    failures += require(
        did.length == 3U &&
            std::to_integer<std::uint8_t>(
                did.payload[1U]) == 0xF1U &&
            std::to_integer<std::uint8_t>(
                did.payload[2U]) == 0x90U,
        "ReadDataByIdentifier builder");
    failures += require(
        dtc.length == 3U &&
            std::to_integer<std::uint8_t>(
                dtc.payload[0U]) == 0x19U &&
            std::to_integer<std::uint8_t>(
                dtc.payload[1U]) == 0x02U,
        "ReadDTCInformation builder");
    failures += require(
        tester.length == 2U &&
            std::to_integer<std::uint8_t>(
                tester.payload[0U]) == 0x3EU,
        "TesterPresent builder");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    failures += require(
        client.start_request(request) ==
            UdsStatus::in_progress &&
            transport.request_length() == 3U &&
            transport.request_byte(0U) == 0x22U,
        "positive request starts through transport");

    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    failures += require(
        client.service(
            reading(std::chrono::milliseconds{1})) ==
            UdsStatus::in_progress,
        "P2 wait begins after TX completion");

    transport.queue_response(
        {0x62U, 0xF1U, 0x90U, 0x53U, 0x41U, 0x43U},
        reading(std::chrono::milliseconds{2}));
    failures += require(
        client.service(
            reading(std::chrono::milliseconds{2})) ==
            UdsStatus::ok &&
            client.has_response(),
        "positive response completes");

    const auto response = client.take_response();
    failures += require(
        response.status == UdsStatus::ok &&
            response.request_sid == 0x22U &&
            response.response_sid == 0x62U &&
            response.length == 6U &&
            response.completion_timestamp.value ==
                std::chrono::milliseconds{2},
        "positive response metadata preserved");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0x1234U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{1})));
    transport.queue_response(
        {0x7FU, 0x22U, 0x31U},
        reading(std::chrono::milliseconds{2}));

    failures += require(
        client.service(
            reading(std::chrono::milliseconds{2})) ==
            UdsStatus::negative_response,
        "negative response completes");
    const auto response = client.take_response();
    failures += require(
        response.status == UdsStatus::negative_response &&
            response.negative_response_code == 0x31U &&
            response.transport_failure ==
                UdsTransportFailure::none,
        "NRC preserved without false transport fault");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{1})));

    transport.queue_response(
        {0x7FU, 0x22U, 0x78U},
        reading(std::chrono::milliseconds{3}));
    failures += require(
        client.service(
            reading(std::chrono::milliseconds{3})) ==
            UdsStatus::in_progress &&
            !client.has_response(),
        "NRC 0x78 enters P2-star wait");

    transport.queue_response(
        {0x62U, 0xF1U, 0x90U, 0x01U},
        reading(std::chrono::milliseconds{30}));
    failures += require(
        client.service(
            reading(std::chrono::milliseconds{30})) ==
            UdsStatus::ok,
        "positive response accepted during P2-star");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{4})));

    failures += require(
        client.service(
            reading(std::chrono::milliseconds{7})) ==
            UdsStatus::timeout_p2,
        "P2 anchored to transport TX timestamp");
    failures += require(
        client.take_response().status ==
            UdsStatus::timeout_p2,
        "P2 timeout exposed as response state");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{1})));
    transport.queue_response(
        {0x7FU, 0x22U, 0x78U},
        reading(std::chrono::milliseconds{3}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{3})));

    failures += require(
        client.service(
            reading(std::chrono::milliseconds{54})) ==
            UdsStatus::timeout_p2_star,
        "P2-star timeout detected");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{1})));
    transport.queue_response(
        {0x62U, 0xF1U, 0x90U},
        reading(std::chrono::milliseconds{7}));

    failures += require(
        client.service(
            reading(std::chrono::milliseconds{8})) ==
            UdsStatus::timeout_p2,
        "late response rejected using RX completion timestamp");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{1})));
    transport.queue_response(
        {0x50U, 0x01U},
        reading(std::chrono::milliseconds{2}));

    failures += require(
        client.service(
            reading(std::chrono::milliseconds{2})) ==
            UdsStatus::protocol_error &&
            client.take_response().transport_failure ==
                UdsTransportFailure::none,
        "mismatched positive SID is UDS protocol error");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    transport.fail_tx(
        transport::DiagnosticTransportStatus::bus_off);
    failures += require(
        client.service(
            reading(std::chrono::milliseconds{1})) ==
            UdsStatus::transport_error,
        "transport TX failure surfaced");
    const auto response = client.take_response();
    failures += require(
        response.transport_failure ==
            UdsTransportFailure::bus_off,
        "bus-off failure classification preserved");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    failures += require(
        client.service(
            reading(
                std::chrono::milliseconds{1},
                time::MonotonicClockDomainId{99U})) ==
            UdsStatus::clock_fault &&
            client.faulted() &&
            transport.reset_count() == 1U,
        "wrong service clock domain fails closed");

    client.reset();
    failures += require(
        !client.faulted(),
        "explicit reset clears UDS clock fault");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_read_data_by_identifier(0xF190U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{10}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{10})));
    transport.queue_response(
        {0x62U, 0xF1U, 0x90U},
        reading(std::chrono::milliseconds{9}));

    failures += require(
        client.service(
            reading(std::chrono::milliseconds{11})) ==
            UdsStatus::clock_fault,
        "backward transaction timestamp fails closed");
  }

  {
    FakeDiagnosticTransport transport;
    UdsClient client{transport, config()};
    const auto request =
        make_diagnostic_session_control(0x03U);

    static_cast<void>(client.start_request(request));
    transport.complete_tx(
        reading(std::chrono::milliseconds{1}));
    static_cast<void>(
        client.service(
            reading(std::chrono::milliseconds{1})));
    transport.queue_response(
        {0x50U, 0x03U, 0x00U, 0x64U, 0x00U, 0x32U},
        reading(std::chrono::milliseconds{2}));

    failures += require(
        client.service(
            reading(std::chrono::milliseconds{2})) ==
            UdsStatus::ok,
        "session control response accepted");
    failures += require(
        client.timing().p2 ==
                std::chrono::milliseconds{100} &&
            client.timing().p2_star ==
                std::chrono::milliseconds{500},
        "server timing parameters promoted after session control");
  }

  if (failures != 0) {
    return 1;
  }

  std::cout << "CORE_V2_UDS_TESTS=PASS\n";
  return 0;
}
