#include "ecu/core_v2/protocol/isotp/isotp_endpoint.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <vector>

namespace {

using namespace ecu::core::v2;
using namespace ecu::core::v2::protocol::isotp;

constexpr time::MonotonicClockDomainId kDomain{77U};

class TestClock final {
 public:
  [[nodiscard]] time::MonotonicClockReading read() const noexcept {
    return {
        time::MonotonicClockStatus::ok,
        kDomain,
        now_,
        uncertainty_};
  }

  template <typename Rep, typename Period>
  void advance(
      const std::chrono::duration<Rep, Period> delta) noexcept {
    now_ +=
        std::chrono::duration_cast<time::MonotonicDuration>(delta);
  }

  void set(const time::MonotonicTime value) noexcept {
    now_ = value;
  }

  void set_uncertainty(
      const time::MonotonicDuration value) noexcept {
    uncertainty_ = value;
  }

 private:
  time::MonotonicTime now_{0};
  time::MonotonicDuration uncertainty_{1};
};

struct SentFrame {
  transport::CanFrame frame{};
  time::MonotonicTime timestamp{0};
};

class TestDriver final : public transport::ICanDriver,
                         public transport::ICanChannelArbiter {
 public:
  TestDriver(
      TestClock& clock,
      const std::uint64_t channel_id) noexcept
      : clock_(clock),
        channel_id_{channel_id} {}

  void connect(TestDriver& peer) noexcept {
    peer_ = &peer;
  }

  [[nodiscard]] transport::CanPhysicalChannelId physical_channel_id()
      const noexcept override {
    return channel_id_;
  }

  [[nodiscard]] transport::ICanChannelArbiter& channel_arbiter()
      noexcept override {
    return *this;
  }

  [[nodiscard]] transport::CanDriverExecutionContract execution_contract()
      const noexcept override {
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
      const void* const owner_token) noexcept override {
    if (channel != channel_id_ ||
        owner_token == nullptr ||
        (owner_ != nullptr && owner_ != owner_token)) {
      return false;
    }
    owner_ = owner_token;
    return true;
  }

  void release(
      const transport::CanPhysicalChannelId channel,
      const void* const owner_token) noexcept override {
    if (channel == channel_id_ &&
        owner_ == owner_token) {
      owner_ = nullptr;
    }
  }

  [[nodiscard]] transport::CanStatus open(
      const transport::CanChannelConfig& config) noexcept override {
    if (open_) {
      return transport::CanStatus::already_open;
    }
    if (!transport::capabilities_support(capabilities(), config)) {
      return transport::CanStatus::unsupported;
    }
    config_ = config;
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
    if (forced_send_status_ != transport::CanStatus::ok) {
      return forced_send_status_;
    }
    if (!open_) {
      return transport::CanStatus::not_open;
    }
    if (!transport::capabilities_support_frame(
            capabilities(), frame)) {
      return transport::CanStatus::unsupported;
    }

    const auto reading = clock_.read();
    sent_.push_back({frame, reading.value});
    if (peer_ != nullptr && peer_->open_) {
      peer_->rx_.push_back({frame, reading});
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

    const auto value = rx_.front();
    rx_.pop_front();
    return {transport::CanStatus::ok, value, 0U};
  }

  void inject(
      const transport::CanFrame& frame,
      const time::MonotonicClockReading timestamp) {
    rx_.push_back({frame, timestamp});
  }

  void set_send_status(
      const transport::CanStatus status) noexcept {
    forced_send_status_ = status;
  }

  [[nodiscard]] const std::vector<SentFrame>& sent()
      const noexcept {
    return sent_;
  }

 private:
  TestClock& clock_;
  transport::CanPhysicalChannelId channel_id_{};
  TestDriver* peer_{nullptr};
  const void* owner_{nullptr};
  bool open_{false};
  transport::CanChannelConfig config_{};
  transport::CanStatus forced_send_status_{
      transport::CanStatus::ok};
  std::deque<transport::ReceivedCanFrame> rx_{};
  std::vector<SentFrame> sent_{};
};

[[nodiscard]] int require(
    const bool condition,
    const char* const message) {
  if (condition) {
    return 0;
  }
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

[[nodiscard]] IsoTpAddress address_a() noexcept {
  return {
      0x700U,
      0x708U,
      transport::CanIdentifierFormat::standard_11_bit};
}

[[nodiscard]] IsoTpAddress address_b() noexcept {
  return {
      0x708U,
      0x700U,
      transport::CanIdentifierFormat::standard_11_bit};
}

[[nodiscard]] IsoTpConfig classic_config() noexcept {
  IsoTpConfig config{};
  config.frame_format = transport::CanFrameFormat::classic;
  config.tx_data_length = 8U;
  config.bit_rate_switch = false;
  config.flow_control_timeout =
      std::chrono::milliseconds{50};
  config.consecutive_frame_timeout =
      std::chrono::milliseconds{50};
  config.timestamp_domain = kDomain;
  config.max_timestamp_uncertainty =
      time::MonotonicDuration{1000};
  return config;
}

[[nodiscard]] IsoTpConfig fd_config() noexcept {
  auto config = classic_config();
  config.frame_format = transport::CanFrameFormat::fd;
  config.tx_data_length = 64U;
  config.bit_rate_switch = true;
  return config;
}

[[nodiscard]] bool start_bus(
    transport::CanBusRuntime& bus,
    transport::ICanFrameSink& sink,
    const IsoTpAddress address,
    const bool fd) noexcept {
  transport::CanFilter filter{};
  filter.identifier = address.rx_id;
  filter.mask =
      address.identifier_format ==
              transport::CanIdentifierFormat::standard_11_bit
          ? 0x7FFU
          : 0x1FFFFFFFU;
  filter.match_standard =
      address.identifier_format ==
      transport::CanIdentifierFormat::standard_11_bit;
  filter.match_extended =
      address.identifier_format ==
      transport::CanIdentifierFormat::extended_29_bit;

  const auto subscription =
      bus.subscribe(
          filter,
          sink,
          {time::MonotonicDuration{100000}});
  if (subscription.status !=
      transport::CanSubscriptionStatus::subscribed) {
    return false;
  }
  if (!bus.freeze_configuration()) {
    return false;
  }

  const transport::CanChannelConfig config{
      500000U,
      fd,
      fd ? 2000000U : 0U,
      transport::CanMode::normal,
      kDomain};
  return bus.start(config) == transport::CanStatus::ok;
}

[[nodiscard]] bool fatal(const IsoTpStatus status) noexcept {
  return status != IsoTpStatus::ok &&
         status != IsoTpStatus::in_progress &&
         status != IsoTpStatus::idle &&
         status != IsoTpStatus::would_block;
}

[[nodiscard]] bool pump(
    IsoTpEndpoint& a,
    transport::CanBusRuntime& bus_a,
    IsoTpEndpoint& b,
    transport::CanBusRuntime& bus_b,
    TestClock& clock,
    const std::size_t iterations,
    const time::MonotonicDuration step) noexcept {
  for (std::size_t index = 0U;
       index < iterations;
       ++index) {
    const auto a_status = a.service(bus_a, clock.read());
    const auto b_status = b.service(bus_b, clock.read());
    if (fatal(a_status) || fatal(b_status)) {
      return false;
    }

    const auto a_poll = bus_a.poll(8U);
    const auto b_poll = bus_b.poll(8U);
    if (a_poll.status != transport::CanStatus::ok ||
        b_poll.status != transport::CanStatus::ok) {
      return false;
    }

    if (!a.tx_busy() && b.has_received()) {
      return true;
    }
    clock.advance(step);
  }
  return false;
}

[[nodiscard]] bool payload_equals(
    const IsoTpReceiveResult& result,
    const std::vector<std::byte>& expected) noexcept {
  if (result.status != IsoTpStatus::ok ||
      result.length != expected.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < expected.size();
       ++index) {
    if (result.payload[index] != expected[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] transport::CanFrame classic_frame(
    const std::uint32_t identifier,
    const std::initializer_list<std::uint8_t> bytes) {
  transport::CanFrame frame{};
  frame.identifier = identifier;
  frame.identifier_format =
      transport::CanIdentifierFormat::standard_11_bit;
  frame.format = transport::CanFrameFormat::classic;
  frame.type = transport::CanFrameType::data;
  frame.length = static_cast<std::uint8_t>(bytes.size());

  std::size_t index = 0U;
  for (const auto value : bytes) {
    frame.payload[index] = static_cast<std::byte>(value);
    ++index;
  }
  return frame;
}

[[nodiscard]] time::MonotonicClockReading reading_at(
    const std::int64_t nanoseconds,
    const std::int64_t uncertainty = 1) noexcept {
  return {
      time::MonotonicClockStatus::ok,
      kDomain,
      time::MonotonicTime{nanoseconds},
      time::MonotonicDuration{uncertainty}};
}

}  // namespace

int main() {
  int failures = 0;

  {
    bool valid = false;
    failures += require(
        decode_stmin(0x05U, valid) ==
                std::chrono::milliseconds{5} &&
            valid,
        "STmin 0x05 = 5 ms");
    failures += require(
        decode_stmin(0xF3U, valid) ==
                std::chrono::microseconds{300} &&
            valid,
        "STmin 0xF3 = 300 us");
    static_cast<void>(decode_stmin(0x80U, valid));
    failures += require(
        !valid,
        "reserved STmin rejected");

    auto invalid = classic_config();
    invalid.timestamp_domain = {};
    failures += require(
        !is_valid_isotp_config(invalid),
        "clock domain is mandatory");
  }

  {
    TestClock clock;
    TestDriver driver_a{clock, 1U};
    TestDriver driver_b{clock, 2U};
    driver_a.connect(driver_b);
    driver_b.connect(driver_a);

    auto config = classic_config();
    IsoTpEndpoint a{address_a(), config};
    IsoTpEndpoint b{address_b(), config};
    IsoTpCanFrameSinkAdapter sink_a{a};
    IsoTpCanFrameSinkAdapter sink_b{b};
    transport::CanBusRuntime bus_a{driver_a};
    transport::CanBusRuntime bus_b{driver_b};

    failures += require(
        start_bus(bus_a, sink_a, address_a(), false) &&
            start_bus(bus_b, sink_b, address_b(), false),
        "classic runtimes start");

    const std::vector<std::byte> payload{
        std::byte{0x22U},
        std::byte{0xF1U},
        std::byte{0x90U}};
    failures += require(
        a.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "classic SF start");
    failures += require(
        pump(
            a,
            bus_a,
            b,
            bus_b,
            clock,
            20U,
            std::chrono::microseconds{100}),
        "classic SF end-to-end");
    failures += require(
        payload_equals(b.take_received(), payload),
        "classic SF payload preserved");
    failures += require(
        driver_a.sent().size() == 1U,
        "classic SF uses one CAN frame");
  }

  {
    TestClock clock;
    TestDriver driver_a{clock, 3U};
    TestDriver driver_b{clock, 4U};
    driver_a.connect(driver_b);
    driver_b.connect(driver_a);

    auto tx_config = classic_config();
    auto rx_config = classic_config();
    rx_config.rx_block_size = 2U;

    IsoTpEndpoint a{address_a(), tx_config};
    IsoTpEndpoint b{address_b(), rx_config};
    IsoTpCanFrameSinkAdapter sink_a{a};
    IsoTpCanFrameSinkAdapter sink_b{b};
    transport::CanBusRuntime bus_a{driver_a};
    transport::CanBusRuntime bus_b{driver_b};
    failures += require(
        start_bus(bus_a, sink_a, address_a(), false) &&
            start_bus(bus_b, sink_b, address_b(), false),
        "classic MF runtimes start");

    std::vector<std::byte> payload(30U);
    for (std::size_t index = 0U;
         index < payload.size();
         ++index) {
      payload[index] =
          static_cast<std::byte>(
              static_cast<std::uint8_t>(index));
    }

    failures += require(
        a.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "classic MF start");
    failures += require(
        pump(
            a,
            bus_a,
            b,
            bus_b,
            clock,
            300U,
            std::chrono::microseconds{100}),
        "classic MF end-to-end with block FC");
    failures += require(
        payload_equals(b.take_received(), payload),
        "classic MF payload preserved");

    std::size_t flow_controls = 0U;
    for (const auto& sent : driver_b.sent()) {
      if ((std::to_integer<std::uint8_t>(
               sent.frame.payload[0U]) &
           0xF0U) == 0x30U) {
        ++flow_controls;
      }
    }
    failures += require(
        flow_controls >= 2U,
        "block size forces repeated FC");
  }

  {
    TestClock clock;
    TestDriver driver_a{clock, 5U};
    TestDriver driver_b{clock, 6U};
    driver_a.connect(driver_b);
    driver_b.connect(driver_a);

    auto config = fd_config();
    IsoTpEndpoint a{address_a(), config};
    IsoTpEndpoint b{address_b(), config};
    IsoTpCanFrameSinkAdapter sink_a{a};
    IsoTpCanFrameSinkAdapter sink_b{b};
    transport::CanBusRuntime bus_a{driver_a};
    transport::CanBusRuntime bus_b{driver_b};
    failures += require(
        start_bus(bus_a, sink_a, address_a(), true) &&
            start_bus(bus_b, sink_b, address_b(), true),
        "FD SF runtimes start");

    std::vector<std::byte> payload(20U);
    for (std::size_t index = 0U;
         index < payload.size();
         ++index) {
      payload[index] =
          static_cast<std::byte>(
              static_cast<std::uint8_t>(0xA0U + index));
    }

    failures += require(
        a.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "FD SF start");
    failures += require(
        pump(
            a,
            bus_a,
            b,
            bus_b,
            clock,
            30U,
            std::chrono::microseconds{100}),
        "FD SF end-to-end");
    failures += require(
        payload_equals(b.take_received(), payload),
        "FD SF payload preserved");
    failures += require(
        driver_a.sent().size() == 1U &&
            driver_a.sent().front().frame.length == 24U,
        "FD SF uses legal 24-byte wire length");
    failures += require(
        std::to_integer<std::uint8_t>(
            driver_a.sent().front().frame.payload[0U]) == 0x00U &&
            std::to_integer<std::uint8_t>(
                driver_a.sent().front().frame.payload[1U]) == 20U,
        "FD SF escape length encoded");
  }

  {
    TestClock clock;
    TestDriver driver_a{clock, 7U};
    TestDriver driver_b{clock, 8U};
    driver_a.connect(driver_b);
    driver_b.connect(driver_a);

    auto tx_config = fd_config();
    auto rx_config = fd_config();
    rx_config.rx_block_size = 1U;
    rx_config.rx_stmin = 0xF3U;

    IsoTpEndpoint a{address_a(), tx_config};
    IsoTpEndpoint b{address_b(), rx_config};
    IsoTpCanFrameSinkAdapter sink_a{a};
    IsoTpCanFrameSinkAdapter sink_b{b};
    transport::CanBusRuntime bus_a{driver_a};
    transport::CanBusRuntime bus_b{driver_b};
    failures += require(
        start_bus(bus_a, sink_a, address_a(), true) &&
            start_bus(bus_b, sink_b, address_b(), true),
        "FD MF runtimes start");

    std::vector<std::byte> payload(180U);
    for (std::size_t index = 0U;
         index < payload.size();
         ++index) {
      payload[index] =
          static_cast<std::byte>(
              static_cast<std::uint8_t>(index & 0xFFU));
    }

    failures += require(
        a.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "FD MF start");
    failures += require(
        pump(
            a,
            bus_a,
            b,
            bus_b,
            clock,
            2000U,
            std::chrono::microseconds{100}),
        "FD MF with block size and STmin");
    failures += require(
        payload_equals(b.take_received(), payload),
        "FD MF payload preserved");

    std::vector<time::MonotonicTime> cf_times;
    for (const auto& sent : driver_a.sent()) {
      if ((std::to_integer<std::uint8_t>(
               sent.frame.payload[0U]) &
           0xF0U) == 0x20U) {
        cf_times.push_back(sent.timestamp);
      }
    }
    failures += require(
        cf_times.size() >= 2U,
        "multiple FD CF frames sent");
    for (std::size_t index = 1U;
         index < cf_times.size();
         ++index) {
      failures += require(
          cf_times[index] - cf_times[index - 1U] >=
              std::chrono::microseconds{300},
          "sender respects received STmin");
    }
  }

  {
    TestClock clock;
    TestDriver driver{clock, 9U};
    auto config = classic_config();
    config.flow_control_timeout =
        std::chrono::milliseconds{10};

    IsoTpEndpoint endpoint{address_a(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_a(), false),
        "timeout runtime starts");

    std::vector<std::byte> payload(
        20U,
        std::byte{0x55U});
    failures += require(
        endpoint.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "timeout transfer start");
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::in_progress,
        "first frame sent and waits for FC");

    clock.advance(std::chrono::milliseconds{11});
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::timeout,
        "flow-control timeout detected");
    failures += require(
        endpoint.last_tx_status() ==
            IsoTpStatus::timeout,
        "timeout stored as TX result");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 30U};
    auto config = classic_config();
    config.flow_control_timeout =
        std::chrono::milliseconds{10};
    IsoTpEndpoint endpoint{address_a(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_a(), false),
        "late-FC runtime starts");

    std::vector<std::byte> payload(
        20U,
        std::byte{0x44U});
    failures += require(
        endpoint.start_send(payload.data(), payload.size()) ==
                IsoTpStatus::in_progress &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::in_progress,
        "late-FC sender waits after FF");

    clock.advance(std::chrono::milliseconds{11});
    driver.inject(
        classic_frame(
            0x708U,
            {0x30U, 0x00U, 0x00U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::timeout &&
            endpoint.last_tx_status() == IsoTpStatus::timeout,
        "late FC cannot revive expired TX session");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 31U};
    auto config = classic_config();
    config.consecutive_frame_timeout =
        std::chrono::milliseconds{10};
    IsoTpEndpoint endpoint{address_b(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_b(), false),
        "late-CF runtime starts");

    driver.inject(
        classic_frame(
            0x700U,
            {0x10U, 0x14U, 1U, 2U, 3U, 4U, 5U, 6U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::in_progress,
        "late-CF receiver starts and sends FC");

    clock.advance(std::chrono::milliseconds{11});
    driver.inject(
        classic_frame(
            0x700U,
            {0x21U, 7U, 8U, 9U, 10U, 11U, 12U, 13U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::timeout &&
            endpoint.last_rx_status() == IsoTpStatus::timeout,
        "late CF rejected from RX timestamp even without interim service");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 32U};
    auto config = classic_config();
    config.rx_block_size = 1U;
    IsoTpEndpoint endpoint{address_b(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_b(), false),
        "early-CF runtime starts");

    driver.inject(
        classic_frame(
            0x700U,
            {0x10U, 0x14U, 1U, 2U, 3U, 4U, 5U, 6U}),
        reading_at(1000));
    driver.inject(
        classic_frame(
            0x700U,
            {0x21U, 7U, 8U, 9U, 10U, 11U, 12U, 13U}),
        reading_at(2000));
    failures += require(
        bus.poll(2U).status == transport::CanStatus::ok &&
            endpoint.service(bus, reading_at(3000)) ==
                IsoTpStatus::protocol_error &&
            !endpoint.pending_control() &&
            driver.sent().empty(),
        "CF before deferred FC is sent aborts RX session");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 10U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_b(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_b(), false),
        "sequence runtime starts");

    driver.inject(
        classic_frame(
            0x700U,
            {0x10U, 0x14U, 1U, 2U, 3U, 4U, 5U, 6U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok &&
            endpoint.pending_control(),
        "RX accepts FF and defers FC");
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::in_progress,
        "deferred FC sent outside callback");

    clock.advance(std::chrono::microseconds{10});
    driver.inject(
        classic_frame(
            0x700U,
            {0x22U, 7U, 8U, 9U, 10U, 11U, 12U, 13U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok,
        "wrong CF dispatched");
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::sequence_error,
        "sequence mismatch surfaced by service");
  }

  {
    TestClock clock;
    TestDriver driver_a{clock, 11U};
    TestDriver driver_b{clock, 12U};
    driver_a.connect(driver_b);
    driver_b.connect(driver_a);

    auto config = classic_config();
    const IsoTpAddress ext_a{
        0x18DAF110U,
        0x18DA10F1U,
        transport::CanIdentifierFormat::extended_29_bit};
    const IsoTpAddress ext_b{
        0x18DA10F1U,
        0x18DAF110U,
        transport::CanIdentifierFormat::extended_29_bit};

    IsoTpEndpoint a{ext_a, config};
    IsoTpEndpoint b{ext_b, config};
    IsoTpCanFrameSinkAdapter sink_a{a};
    IsoTpCanFrameSinkAdapter sink_b{b};
    transport::CanBusRuntime bus_a{driver_a};
    transport::CanBusRuntime bus_b{driver_b};
    failures += require(
        start_bus(bus_a, sink_a, ext_a, false) &&
            start_bus(bus_b, sink_b, ext_b, false),
        "29-bit runtimes start");

    const std::vector<std::byte> payload{
        std::byte{0x10U},
        std::byte{0x03U}};
    failures += require(
        a.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "29-bit SF start");
    failures += require(
        pump(
            a,
            bus_a,
            b,
            bus_b,
            clock,
            20U,
            std::chrono::microseconds{100}),
        "29-bit SF end-to-end");
    failures += require(
        payload_equals(b.take_received(), payload),
        "29-bit addressing preserved");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 13U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_a(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_a(), false),
        "overflow runtime starts");

    std::vector<std::byte> payload(
        20U,
        std::byte{0x11U});
    failures += require(
        endpoint.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::in_progress,
        "overflow sender waits after FF");

    clock.advance(std::chrono::microseconds{10});
    driver.inject(
        classic_frame(
            0x708U,
            {0x32U, 0x00U, 0x00U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::flow_control_overflow,
        "FC Overflow terminates sender");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 14U};
    auto config = classic_config();
    config.max_wait_frames = 1U;
    IsoTpEndpoint endpoint{address_a(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_a(), false),
        "wait-frame runtime starts");

    std::vector<std::byte> payload(
        20U,
        std::byte{0x22U});
    failures += require(
        endpoint.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::in_progress,
        "wait sender starts");

    clock.advance(std::chrono::microseconds{10});
    driver.inject(
        classic_frame(
            0x708U,
            {0x31U, 0x00U, 0x00U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::in_progress,
        "first FC Wait accepted");

    clock.advance(std::chrono::microseconds{10});
    driver.inject(
        classic_frame(
            0x708U,
            {0x31U, 0x00U, 0x00U}),
        clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok &&
            endpoint.service(bus, clock.read()) ==
                IsoTpStatus::timeout,
        "excess FC Wait rejected");
  }

  {
    TestClock clock;
    TestDriver driver_a{clock, 15U};
    TestDriver driver_b{clock, 16U};
    driver_a.connect(driver_b);
    driver_b.connect(driver_a);

    auto tx_config = classic_config();
    auto rx_config = classic_config();
    rx_config.rx_block_size = 8U;

    IsoTpEndpoint a{address_a(), tx_config};
    IsoTpEndpoint b{address_b(), rx_config};
    IsoTpCanFrameSinkAdapter sink_a{a};
    IsoTpCanFrameSinkAdapter sink_b{b};
    transport::CanBusRuntime bus_a{driver_a};
    transport::CanBusRuntime bus_b{driver_b};
    failures += require(
        start_bus(bus_a, sink_a, address_a(), false) &&
            start_bus(bus_b, sink_b, address_b(), false),
        "4095-byte runtimes start");

    std::vector<std::byte> payload(kMaxPayloadSize);
    for (std::size_t index = 0U;
         index < payload.size();
         ++index) {
      payload[index] =
          static_cast<std::byte>(
              static_cast<std::uint8_t>(
                  (index * 17U) & 0xFFU));
    }

    failures += require(
        a.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "4095-byte transfer start");
    failures += require(
        pump(
            a,
            bus_a,
            b,
            bus_b,
            clock,
            10000U,
            std::chrono::microseconds{100}),
        "4095-byte transfer completes");
    failures += require(
        payload_equals(b.take_received(), payload),
        "4095-byte payload preserved across sequence wrap");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 17U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_b(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_b(), false),
        "extended-length rejection runtime starts");

    auto frame = classic_frame(
        0x700U,
        {0x10U, 0x00U, 0x00U, 0x00U, 0x10U, 0x00U, 0x00U, 0x00U});
    driver.inject(frame, clock.read());
    failures += require(
        bus.poll(1U).status == transport::CanStatus::ok,
        "extended FF dispatched");
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::payload_too_large,
        "unsupported extended FF rejected");
    failures += require(
        endpoint.pending_control(),
        "overflow FC remains queued after error event");
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::ok,
        "overflow FC emitted on following service");
    failures += require(
        !driver.sent().empty() &&
            (std::to_integer<std::uint8_t>(
                 driver.sent().back().frame.payload[0U]) &
             0x0FU) == 0x02U,
        "overflow FC uses FS=Overflow");
  }

  {
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_a(), config};
    std::vector<std::byte> too_large(
        kMaxPayloadSize + 1U);
    failures += require(
        endpoint.start_send(
            too_large.data(),
            too_large.size()) ==
            IsoTpStatus::payload_too_large,
        "payload above 4095 rejected");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 18U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_b(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_b(), false),
        "time-domain separation runtime starts");

    clock.set(time::MonotonicTime{1000000});
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::ok,
        "service time accepted");

    const auto sf = classic_frame(
        0x700U,
        {0x03U, 0x22U, 0xF1U, 0x90U});
    endpoint.on_can_frame({
        sf,
        reading_at(500000)});
    failures += require(
        endpoint.has_received() &&
            endpoint.last_rx_status() == IsoTpStatus::ok,
        "older queued RX timestamp does not conflict with service clock");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 19U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_b(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_b(), false),
        "RX monotonicity runtime starts");

    const auto sf = classic_frame(
        0x700U,
        {0x01U, 0xAAU});
    endpoint.on_can_frame({
        sf,
        reading_at(2000)});
    static_cast<void>(endpoint.take_received());
    endpoint.on_can_frame({
        sf,
        reading_at(1000)});
    failures += require(
        endpoint.service(bus, reading_at(3000)) ==
            IsoTpStatus::clock_fault,
        "backward RX timestamp latches clock fault");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 20U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_a(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_a(), false),
        "service monotonicity runtime starts");

    failures += require(
        endpoint.service(bus, reading_at(2000)) ==
            IsoTpStatus::ok,
        "first service timestamp accepted");
    failures += require(
        endpoint.service(bus, reading_at(1000)) ==
            IsoTpStatus::clock_fault,
        "backward service timestamp latches clock fault");
    const std::byte byte{0x01U};
    failures += require(
        endpoint.start_send(&byte, 1U) ==
            IsoTpStatus::clock_fault,
        "clock fault blocks new TX");
    endpoint.reset();
    failures += require(
        endpoint.start_send(&byte, 1U) ==
            IsoTpStatus::in_progress,
        "explicit reset clears endpoint clock fault");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 21U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_b(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_b(), false),
        "malformed-frame runtime starts");

    auto malformed = classic_frame(
        0x700U,
        {0x01U, 0xAAU});
    malformed.length = 9U;
    endpoint.on_can_frame({
        malformed,
        clock.read()});
    failures += require(
        endpoint.service(bus, clock.read()) ==
            IsoTpStatus::protocol_error,
        "matching malformed CAN frame rejected before ISO-TP parse");
  }

  {
    TestClock clock;
    TestDriver driver{clock, 22U};
    auto config = classic_config();
    IsoTpEndpoint endpoint{address_a(), config};
    IsoTpCanFrameSinkAdapter sink{endpoint};
    transport::CanBusRuntime bus{driver};
    failures += require(
        start_bus(bus, sink, address_a(), false),
        "would-block runtime starts");

    const std::vector<std::byte> payload{
        std::byte{0x22U}};
    failures += require(
        endpoint.start_send(payload.data(), payload.size()) ==
            IsoTpStatus::in_progress,
        "would-block SF starts");
    driver.set_send_status(
        transport::CanStatus::would_block);
    failures += require(
        endpoint.service(bus, clock.read()) ==
                IsoTpStatus::would_block &&
            endpoint.tx_busy(),
        "would-block preserves pending TX");
    driver.set_send_status(transport::CanStatus::ok);
    failures += require(
        endpoint.service(bus, clock.read()) ==
                IsoTpStatus::ok &&
            !endpoint.tx_busy(),
        "pending TX retries exactly once");
  }

  if (failures != 0) {
    return 1;
  }

  std::cout << "CORE_V2_ISOTP_TESTS=PASS\n";
  return 0;
}
