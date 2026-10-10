#include "ecu/core_v2/protocol/isotp/isotp_diagnostic_transport.hpp"
#include "ecu/core_v2/protocol/uds/uds_client.hpp"
#include "ecu/core_v2/protocol/uds/uds_services.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <vector>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::isotp;
using namespace ecu::core::v2::protocol::uds;

constexpr time::MonotonicClockDomainId kDomain{77U};

class TestClock final {
 public:
  [[nodiscard]] time::MonotonicClockReading read() const noexcept {
    return {
        time::MonotonicClockStatus::ok,
        kDomain,
        now_,
        time::MonotonicDuration{1}};
  }

  void advance(
      const time::MonotonicDuration delta) noexcept {
    now_ += delta;
  }

 private:
  time::MonotonicTime now_{0};
};

class TestDriver final : public transport::ICanDriver,
                         public transport::ICanChannelArbiter {
 public:
  TestDriver(
      TestClock& clock,
      const std::uint64_t channel) noexcept
      : clock_(clock),
        channel_{channel} {}

  void connect(TestDriver& peer) noexcept {
    peer_ = &peer;
  }

  [[nodiscard]] transport::CanPhysicalChannelId
  physical_channel_id() const noexcept override {
    return channel_;
  }

  [[nodiscard]] transport::ICanChannelArbiter&
  channel_arbiter() noexcept override {
    return *this;
  }

  [[nodiscard]] transport::CanDriverExecutionContract
  execution_contract() const noexcept override {
    const time::MonotonicDuration bound{1000};
    return {
        bound,
        bound,
        bound,
        bound,
        bound,
        bound,
        bound};
  }

  [[nodiscard]] transport::CanCapabilities capabilities()
      const noexcept override {
    return {true, true, true, false, 64U};
  }

  [[nodiscard]] bool try_acquire(
      const transport::CanPhysicalChannelId channel,
      const void* const owner) noexcept override {
    if (channel != channel_ ||
        owner == nullptr ||
        (owner_ != nullptr && owner_ != owner)) {
      return false;
    }
    owner_ = owner;
    return true;
  }

  void release(
      const transport::CanPhysicalChannelId channel,
      const void* const owner) noexcept override {
    if (channel == channel_ && owner_ == owner) {
      owner_ = nullptr;
    }
  }

  [[nodiscard]] transport::CanStatus open(
      const transport::CanChannelConfig& config) noexcept override {
    if (open_) {
      return transport::CanStatus::already_open;
    }
    if (!transport::capabilities_support(
            capabilities(),
            config)) {
      return transport::CanStatus::unsupported;
    }
    open_ = true;
    return transport::CanStatus::ok;
  }

  void close() noexcept override {
    open_ = false;
    rx_.clear();
  }

  [[nodiscard]] bool is_open() const noexcept override {
    return open_;
  }

  [[nodiscard]] transport::CanStatus try_send(
      const transport::CanFrame& frame) noexcept override {
    if (!open_) {
      return transport::CanStatus::not_open;
    }
    if (!transport::capabilities_support_frame(
            capabilities(),
            frame)) {
      return transport::CanStatus::unsupported;
    }
    if (peer_ != nullptr && peer_->open_) {
      peer_->rx_.push_back({frame, clock_.read()});
    }
    return transport::CanStatus::ok;
  }

  [[nodiscard]] transport::CanReceiveResult try_receive()
      noexcept override {
    if (!open_) {
      return {transport::CanStatus::not_open, {}, 0U};
    }
    if (rx_.empty()) {
      return {transport::CanStatus::would_block, {}, 0U};
    }
    const auto result = rx_.front();
    rx_.pop_front();
    return {transport::CanStatus::ok, result, 0U};
  }

 private:
  TestClock& clock_;
  transport::CanPhysicalChannelId channel_{};
  TestDriver* peer_{nullptr};
  const void* owner_{nullptr};
  bool open_{false};
  std::deque<transport::ReceivedCanFrame> rx_{};
};

[[nodiscard]] IsoTpAddress client_address() noexcept {
  return {
      0x700U,
      0x708U,
      transport::CanIdentifierFormat::standard_11_bit};
}

[[nodiscard]] IsoTpAddress server_address() noexcept {
  return {
      0x708U,
      0x700U,
      transport::CanIdentifierFormat::standard_11_bit};
}

[[nodiscard]] IsoTpConfig isotp_config() noexcept {
  IsoTpConfig result{};
  result.frame_format =
      transport::CanFrameFormat::classic;
  result.tx_data_length = 8U;
  result.flow_control_timeout =
      std::chrono::milliseconds{20};
  result.consecutive_frame_timeout =
      std::chrono::milliseconds{20};
  result.timestamp_domain = kDomain;
  result.max_timestamp_uncertainty =
      std::chrono::microseconds{10};
  return result;
}

[[nodiscard]] UdsClientConfig uds_config() noexcept {
  UdsClientConfig result{};
  result.timing.p2 = std::chrono::milliseconds{5};
  result.timing.p2_star =
      std::chrono::milliseconds{50};
  result.timestamp_domain = kDomain;
  result.max_timestamp_uncertainty =
      std::chrono::microseconds{10};
  return result;
}

[[nodiscard]] bool start_bus(
    transport::CanBusRuntime& bus,
    transport::ICanFrameSink& sink,
    const IsoTpAddress address) noexcept {
  transport::CanFilter filter{};
  filter.identifier = address.rx_id;
  filter.mask = 0x7FFU;
  filter.match_standard = true;
  filter.match_extended = false;

  const auto subscription =
      bus.subscribe(
          filter,
          sink,
          {std::chrono::microseconds{100}});
  if (subscription.status !=
      transport::CanSubscriptionStatus::subscribed) {
    return false;
  }
  if (!bus.freeze_configuration()) {
    return false;
  }

  const transport::CanChannelConfig config{
      500000U,
      false,
      0U,
      transport::CanMode::normal,
      kDomain};
  return bus.start(config) ==
         transport::CanStatus::ok;
}

[[nodiscard]] bool allowed_isotp_status(
    const IsoTpStatus status) noexcept {
  return status == IsoTpStatus::ok ||
         status == IsoTpStatus::idle ||
         status == IsoTpStatus::in_progress ||
         status == IsoTpStatus::would_block;
}

[[nodiscard]] bool allowed_uds_status(
    const UdsStatus status) noexcept {
  return status == UdsStatus::ok ||
         status == UdsStatus::idle ||
         status == UdsStatus::in_progress;
}

[[nodiscard]] bool same_request(
    const IsoTpReceiveResult& request,
    const UdsRequest& expected) noexcept {
  if (request.status != IsoTpStatus::ok ||
      request.length != expected.length) {
    return false;
  }
  for (std::size_t index = 0U;
       index < expected.length;
       ++index) {
    if (request.payload[index] !=
        expected.payload[index]) {
      return false;
    }
  }
  return true;
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

  TestClock clock;
  TestDriver client_driver{clock, 1U};
  TestDriver server_driver{clock, 2U};
  client_driver.connect(server_driver);
  server_driver.connect(client_driver);

  const auto tp_config = isotp_config();
  IsoTpEndpoint client_endpoint{
      client_address(),
      tp_config};
  IsoTpEndpoint server_endpoint{
      server_address(),
      tp_config};
  IsoTpCanFrameSinkAdapter client_sink{
      client_endpoint};
  IsoTpCanFrameSinkAdapter server_sink{
      server_endpoint};

  transport::CanBusRuntime client_bus{client_driver};
  transport::CanBusRuntime server_bus{server_driver};

  failures += require(
      start_bus(
          client_bus,
          client_sink,
          client_address()) &&
          start_bus(
              server_bus,
              server_sink,
              server_address()),
      "CAN runtimes start");

  IsoTpDiagnosticTransport diagnostic_transport{
      client_endpoint,
      client_bus};
  UdsClient client{
      diagnostic_transport,
      uds_config()};

  const auto request =
      make_read_data_by_identifier(0xF190U);
  failures += require(
      client.start_request(request) ==
          UdsStatus::in_progress,
      "UDS request starts over ISO-TP adapter");

  std::vector<std::byte> response(24U);
  response[0U] = std::byte{0x62U};
  response[1U] = std::byte{0xF1U};
  response[2U] = std::byte{0x90U};
  for (std::size_t index = 3U;
       index < response.size();
       ++index) {
    response[index] =
        static_cast<std::byte>(
            static_cast<std::uint8_t>(
                0x40U + index));
  }

  bool response_started = false;
  bool completed = false;

  for (std::size_t iteration = 0U;
       iteration < 2000U;
       ++iteration) {
    const auto now = clock.read();

    const auto client_status = client.service(now);
    if (!allowed_uds_status(client_status)) {
      break;
    }

    const auto server_poll = server_bus.poll(8U);
    if (server_poll.status != transport::CanStatus::ok &&
        server_poll.status !=
            transport::CanStatus::would_block) {
      break;
    }

    const auto server_service =
        server_endpoint.service(
            server_bus,
            now);
    if (!allowed_isotp_status(server_service)) {
      break;
    }

    if (!response_started &&
        server_endpoint.has_received()) {
      const auto received =
          server_endpoint.take_received();
      if (!same_request(received, request)) {
        break;
      }
      if (server_endpoint.start_send(
              response.data(),
              response.size()) !=
          IsoTpStatus::in_progress) {
        break;
      }
      response_started = true;
    }

    if (response_started) {
      const auto tx_status =
          server_endpoint.service(
              server_bus,
              now);
      if (!allowed_isotp_status(tx_status)) {
        break;
      }
    }

    const auto client_poll = client_bus.poll(8U);
    if (client_poll.status != transport::CanStatus::ok &&
        client_poll.status !=
            transport::CanStatus::would_block) {
      break;
    }

    const auto second_client_status =
        client.service(now);
    if (!allowed_uds_status(second_client_status)) {
      break;
    }

    if (client.has_response()) {
      completed = true;
      break;
    }

    clock.advance(
        std::chrono::microseconds{100});
  }

  failures += require(
      response_started,
      "server received UDS request through ISO-TP");
  failures += require(
      completed,
      "multi-frame UDS response completes through ISO-TP");

  if (completed) {
    const auto result = client.take_response();
    failures += require(
        result.status == UdsStatus::ok &&
            result.request_sid == 0x22U &&
            result.response_sid == 0x62U &&
            result.length == response.size(),
        "UDS response metadata survives end-to-end");
    for (std::size_t index = 0U;
         index < response.size();
         ++index) {
      failures += require(
          result.payload[index] == response[index],
          "UDS multi-frame payload preserved");
    }
  }

  if (failures != 0) {
    return 1;
  }

  std::cout << "CORE_V2_UDS_ISOTP_E2E_TESTS=PASS\n";
  return 0;
}
