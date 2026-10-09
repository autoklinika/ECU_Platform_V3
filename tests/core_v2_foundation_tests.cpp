#include "ecu/core_v2/domain/product_scope.hpp"
#include "ecu/core_v2/domain/device_under_test.hpp"
#include "ecu/core_v2/safety/deadline_watchdog.hpp"
#include "ecu/core_v2/transport/can_bus_runtime.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <iostream>
#include <limits>
#include <type_traits>

namespace allocation_probe {
std::size_t count = 0;
}

void* operator new(const std::size_t size) {
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    ++allocation_probe::count;
    return memory;
  }
  throw std::bad_alloc{};
}

void* operator new[](const std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace ecu::core::v2;

static_assert(!std::is_copy_constructible_v<transport::CanBusRuntime>);
static_assert(!std::is_copy_assignable_v<transport::CanBusRuntime>);
static_assert(!std::is_move_constructible_v<transport::CanBusRuntime>);
static_assert(!std::is_move_assignable_v<transport::CanBusRuntime>);
static_assert(!std::is_copy_constructible_v<safety::DeadlineWatchdog>);
static_assert(!std::is_move_constructible_v<safety::DeadlineWatchdog>);

inline constexpr time::MonotonicClockDomainId kTestClockDomain{0xA11U};
inline constexpr transport::CanSinkExecutionContract kSinkExecution{
    time::MonotonicDuration{100}};
inline constexpr transport::CanDriverExecutionContract kDriverExecution{
    time::MonotonicDuration{500},
    time::MonotonicDuration{500},
    time::MonotonicDuration{5000},
    time::MonotonicDuration{5000},
    time::MonotonicDuration{1000},
    time::MonotonicDuration{1000},
    time::MonotonicDuration{100}};

class FakeClock final : public time::IMonotonicClock {
 public:
  [[nodiscard]] time::MonotonicClockProperties properties()
      const noexcept override {
    return properties_value;
  }

  [[nodiscard]] time::MonotonicClockReading read()
      const noexcept override {
    return {
        status,
        reading_domain,
        now_,
        uncertainty};
  }

  void set(time::MonotonicTime value) noexcept { now_ = value; }

  void advance(const time::MonotonicDuration delta) noexcept {
    now_ += delta;
  }

  time::MonotonicClockProperties properties_value{
      kTestClockDomain,
      time::MonotonicDuration{1},
      time::MonotonicDuration{100},
      time::MonotonicDuration{1},
      true};
  time::MonotonicClockStatus status{time::MonotonicClockStatus::ok};
  time::MonotonicClockDomainId reading_domain{kTestClockDomain};
  time::MonotonicDuration uncertainty{1};

 private:
  time::MonotonicTime now_{0};
};

class FakeCanArbiter final : public transport::ICanChannelArbiter {
 public:
  [[nodiscard]] bool try_acquire(
      const transport::CanPhysicalChannelId channel,
      const void* owner_token) noexcept override {
    ++attempts;
    if (!channel.valid() || owner_token == nullptr) {
      return false;
    }
    if (channel_.valid() && channel_ != channel) {
      return false;
    }
    if (owner_ != nullptr && owner_ != owner_token) {
      return false;
    }
    channel_ = channel;
    if (owner_ == nullptr) {
      owner_ = owner_token;
      ++acquires;
    }
    return true;
  }

  void release(
      const transport::CanPhysicalChannelId channel,
      const void* owner_token) noexcept override {
    if (channel == channel_ && owner_ == owner_token &&
        owner_token != nullptr) {
      owner_ = nullptr;
      ++releases;
    }
  }

  std::size_t attempts{0U};
  std::size_t acquires{0U};
  std::size_t releases{0U};

 private:
  transport::CanPhysicalChannelId channel_{};
  const void* owner_{nullptr};
};

class FakeCanDriver final : public transport::ICanDriver,
                            public transport::ICanChannelArbiter {
 public:
  [[nodiscard]] transport::CanPhysicalChannelId physical_channel_id()
      const noexcept override {
    return physical_channel_id_value;
  }

  [[nodiscard]] transport::ICanChannelArbiter& channel_arbiter()
      noexcept override {
    return arbiter_override != nullptr ? *arbiter_override : *this;
  }

  [[nodiscard]] transport::CanDriverExecutionContract execution_contract()
      const noexcept override {
    return execution_contract_value;
  }

  [[nodiscard]] bool try_acquire(
      const transport::CanPhysicalChannelId channel,
      const void* owner_token) noexcept override {
    ++lease_attempts;
    if (channel != physical_channel_id_value ||
        owner_token == nullptr ||
        (lease_owner_ != nullptr && lease_owner_ != owner_token)) {
      return false;
    }
    if (lease_owner_ == nullptr) {
      lease_owner_ = owner_token;
      ++lease_acquires;
    }
    return true;
  }

  void release(
      const transport::CanPhysicalChannelId channel,
      const void* owner_token) noexcept override {
    if (reenter_stop_on_release && reentrant_bus != nullptr) {
      reentrant_bus->stop();
    }
    if (channel == physical_channel_id_value &&
        owner_token != nullptr && lease_owner_ == owner_token) {
      lease_owner_ = nullptr;
      ++lease_releases;
    }
  }

  [[nodiscard]] transport::CanCapabilities capabilities()
      const noexcept override {
    return capabilities_value;
  }

  [[nodiscard]] transport::CanStatus open(
      const transport::CanChannelConfig& config) noexcept override {
    ++opens;
    if (reenter_stop_on_open && reentrant_bus != nullptr) {
      reentrant_bus->stop();
    }
    partial_resources = 1;
    if (open_status != transport::CanStatus::ok) {
      open_ = false;
      partial_resources = 0;
      return open_status;
    }
    if (open_) {
      open_ = false;
      partial_resources = 0;
      return transport::CanStatus::already_open;
    }
    if (!transport::capabilities_support(capabilities(), config)) {
      partial_resources = 0;
      return transport::CanStatus::unsupported;
    }
    config_ = config;
    open_ = true;
    return transport::CanStatus::ok;
  }

  void close() noexcept override {
    ++closes;
    if (reenter_stop_on_close && reentrant_bus != nullptr) {
      reentrant_bus->stop();
    }
    open_ = false;
    partial_resources = 0;
    rx_read_ = rx_write_;
    rx_dropped_report = 0U;
  }

  [[nodiscard]] bool is_open() const noexcept override {
    return open_;
  }

  [[nodiscard]] transport::CanStatus try_send(
      const transport::CanFrame& frame) noexcept override {
    ++sends;
    if (reenter_stop_on_send && reentrant_bus != nullptr) {
      reentrant_bus->stop();
    }
    if (send_status != transport::CanStatus::ok) {
      if (close_on_send_failure) {
        open_ = false;
        partial_resources = 0;
        close_on_send_failure = false;
      }
      return send_status;
    }
    if (!open_) {
      return transport::CanStatus::not_open;
    }
    if (!transport::capabilities_support_frame(
            capabilities(), frame)) {
      return transport::CanStatus::unsupported;
    }
    last_tx_ = frame;
    ++tx_count_;
    return transport::CanStatus::ok;
  }

  [[nodiscard]] transport::CanReceiveResult try_receive()
      noexcept override {
    ++receives;
    if (reenter_stop_on_receive && reentrant_bus != nullptr) {
      reentrant_bus->stop();
    }

    const auto dropped = rx_dropped_report;
    rx_dropped_report = 0U;

    if (receive_status != transport::CanStatus::ok) {
      return {receive_status, {}, dropped};
    }
    if (!open_) {
      return {transport::CanStatus::not_open, {}, dropped};
    }

    if (rx_read_ == rx_write_) {
      return {transport::CanStatus::would_block, {}, dropped};
    }

    auto value = rx_[rx_read_ % rx_.size()];
    ++rx_read_;

    if (auto_timestamp &&
        value.timestamp.status ==
            time::MonotonicClockStatus::unavailable) {
      value.timestamp = {
          time::MonotonicClockStatus::ok,
          config_.timestamp_domain,
          time::MonotonicTime{
              static_cast<std::int64_t>(++timestamp_counter_)},
          time::MonotonicDuration{1}};
    }

    return {transport::CanStatus::ok, value, dropped};
  }

  bool inject(const transport::ReceivedCanFrame& frame) noexcept {
    if (rx_write_ - rx_read_ >= rx_.size()) {
      return false;
    }
    rx_[rx_write_ % rx_.size()] = frame;
    ++rx_write_;
    return true;
  }

  [[nodiscard]] std::size_t tx_count() const noexcept {
    return tx_count_;
  }

  std::size_t partial_resources{0};
  std::size_t lease_attempts{0};
  std::size_t lease_acquires{0};
  std::size_t lease_releases{0};
  std::size_t opens{0};
  std::size_t closes{0};
  std::size_t sends{0};
  std::size_t receives{0};
  bool close_on_send_failure{false};
  bool auto_timestamp{true};
  bool reenter_stop_on_open{false};
  bool reenter_stop_on_send{false};
  bool reenter_stop_on_receive{false};
  bool reenter_stop_on_close{false};
  bool reenter_stop_on_release{false};
  transport::CanBusRuntime* reentrant_bus{nullptr};
  transport::ICanChannelArbiter* arbiter_override{nullptr};
  transport::CanPhysicalChannelId physical_channel_id_value{1U};
  transport::CanDriverExecutionContract execution_contract_value{
      kDriverExecution};
  transport::CanCapabilities capabilities_value{true, true, true, true, 64U};
  transport::CanStatus open_status{transport::CanStatus::ok};
  transport::CanStatus send_status{transport::CanStatus::ok};
  transport::CanStatus receive_status{transport::CanStatus::ok};
  std::uint32_t rx_dropped_report{0U};

 private:
  const void* lease_owner_{nullptr};
  bool open_{false};
  transport::CanChannelConfig config_{};
  std::array<transport::ReceivedCanFrame, 8U> rx_{};
  std::size_t rx_read_{0U};
  std::size_t rx_write_{0U};
  transport::CanFrame last_tx_{};
  std::size_t tx_count_{0U};
  std::uint64_t timestamp_counter_{0U};
};

class CountingSink final : public transport::ICanFrameSink {
 public:
  void on_can_frame(
      const transport::ReceivedCanFrame& frame) noexcept override {
    ++count;
    last_identifier = frame.frame.identifier;
  }

  std::size_t count{0U};
  std::uint32_t last_identifier{0U};
};

class ReentrantSink final : public transport::ICanFrameSink {
 public:
  transport::CanBusRuntime* bus{nullptr};
  bool rejected{false};
  void on_can_frame(const transport::ReceivedCanFrame& frame) noexcept override {
    rejected =
        bus->poll(1).status == transport::CanStatus::busy &&
        bus->send(frame.frame) == transport::CanStatus::busy &&
        bus->start(
            {500000, false, 0, transport::CanMode::normal,
             kTestClockDomain}) ==
            transport::CanStatus::busy;
    rejected = rejected && bus->recover() == transport::CanStatus::busy;
    bus->stop();
    rejected = rejected && bus->state() == transport::CanBusState::running;
  }
};

int require(const bool condition, const char* message) {
  if (condition) {
    return 0;
  }

  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

}  // namespace

int main() {
  int failures = 0;

  failures += require(static_cast<unsigned>(domain::MachineDomain::truck) == 0 &&
                          static_cast<unsigned>(domain::MachineDomain::agri) == 1 &&
                          static_cast<unsigned>(domain::MachineDomain::ohv) == 2,
                      "exact product enum values");
  for (unsigned value = 0; value <= 255; ++value) {
    const auto mask = static_cast<domain::DomainMask>(value);
    failures += require(domain::is_supported_domain_mask(mask) == (value >= 1 && value <= 7),
                        "only masks 1..7 supported");
    const auto expected = value < 3 ? (1U << value) : 0U;
    failures += require(domain::domain_mask(static_cast<domain::MachineDomain>(value)) == expected,
                        "exact domain mapping and all invalid values");
  }
  failures += require(domain::kTruck == 0x01U, "truck mask = 0x01");
  failures += require(domain::kAgri == 0x02U, "agri mask = 0x02");
  failures += require(domain::kOhv == 0x04U, "ohv mask = 0x04");
  failures += require(domain::kAllSupported == 0x07U, "all masks = 0x07");
  failures += require(
      domain::is_supported_domain_mask(domain::kTruck) &&
          domain::is_supported_domain_mask(domain::kAgri) &&
          domain::is_supported_domain_mask(domain::kOhv) &&
          domain::is_supported_domain_mask(domain::kAllSupported) &&
          !domain::is_supported_domain_mask(0U) &&
          domain::domain_mask(
              static_cast<domain::MachineDomain>(255U)) == 0U,
      "domain-mask invariant");

  {
    FakeClock clock;
    safety::DeadlineWatchdog watchdog{clock};

    failures += require(
        watchdog.arm(std::chrono::milliseconds{10}) ==
            safety::DeadlineWatchdogStatus::ok,
        "watchdog arm");

    clock.advance(std::chrono::milliseconds{11});

    failures += require(
        watchdog.kick() ==
                safety::DeadlineWatchdogStatus::expired &&
            watchdog.state() ==
                safety::DeadlineWatchdogState::expired,
        "late watchdog kick fails closed");

    failures += require(
        watchdog.kick() ==
            safety::DeadlineWatchdogStatus::expired,
        "expired watchdog cannot be revived by kick");
  }

  {
    FakeCanDriver driver;
    CountingSink j1939_sink;
    CountingSink trace_sink;
    transport::CanBusRuntime bus{driver};

    const transport::CanFilter extended_all{
        0U,
        0U,
        false,
        true};
    const transport::CanFilter all_frames{
        0U,
        0U,
        true,
        true};

    const auto j1939_sub =
        bus.subscribe(extended_all, j1939_sink, kSinkExecution);
    const auto trace_sub =
        bus.subscribe(all_frames, trace_sink, kSinkExecution);

    failures += require(
        j1939_sub.status ==
                transport::CanSubscriptionStatus::subscribed &&
            trace_sub.status ==
                transport::CanSubscriptionStatus::subscribed &&
            bus.subscription_count() == 2U,
        "shared-bus subscriptions");

    failures += require(
        bus.freeze_configuration() &&
            bus.state() == transport::CanBusState::ready,
        "bus configuration freeze");

    CountingSink late_sink;
    failures += require(
        bus.subscribe(all_frames, late_sink, kSinkExecution).status ==
            transport::CanSubscriptionStatus::configuration_frozen,
        "registration blocked after freeze");

    const transport::CanChannelConfig config{
        500000U,
        true,
        2000000U,
        transport::CanMode::normal,
        kTestClockDomain};

    failures += require(
        bus.start(config) == transport::CanStatus::ok &&
            bus.state() == transport::CanBusState::running,
        "bus start");

    transport::ReceivedCanFrame extended{};
    extended.frame.identifier = 0x18FEEE01U;
    extended.frame.identifier_format =
        transport::CanIdentifierFormat::extended_29_bit;
    extended.frame.format = transport::CanFrameFormat::classic;
    extended.frame.length = 8U;
    extended.timestamp = {
        time::MonotonicClockStatus::ok,
        kTestClockDomain,
        time::MonotonicTime{100000},
        time::MonotonicDuration{1}};

    transport::ReceivedCanFrame standard{};
    standard.frame.identifier = 0x7E8U;
    standard.frame.identifier_format =
        transport::CanIdentifierFormat::standard_11_bit;
    standard.frame.format = transport::CanFrameFormat::classic;
    standard.frame.length = 8U;
    standard.timestamp = {
        time::MonotonicClockStatus::ok,
        kTestClockDomain,
        time::MonotonicTime{200000},
        time::MonotonicDuration{1}};

    failures += require(
        driver.inject(extended) && driver.inject(standard),
        "inject shared-bus frames");

    const auto poll = bus.poll(8U);

    failures += require(
        poll.status == transport::CanStatus::ok &&
            poll.frames_received == 2U &&
            poll.deliveries == 3U,
        "central bus dispatch accounting");

    failures += require(
        j1939_sink.count == 1U &&
            j1939_sink.last_identifier == 0x18FEEE01U &&
            trace_sink.count == 2U,
        "J1939 and trace consumers coexist without stealing frames");

    failures += require(
        bus.send(extended.frame) == transport::CanStatus::ok &&
            driver.tx_count() == 1U,
        "centralized TX path");
  }

  {
    using namespace transport;
    CanFrame frame{};
    for (unsigned value = 2; value <= 255; ++value) {
      frame.identifier_format = static_cast<CanIdentifierFormat>(value);
      failures += require(!is_valid_can_frame(frame), "invalid identifier enum");
      frame = {};
      frame.format = static_cast<CanFrameFormat>(value);
      failures += require(!is_valid_can_frame(frame), "invalid format enum");
      frame = {};
      frame.type = static_cast<CanFrameType>(value);
      failures += require(!is_valid_can_frame(frame), "invalid type enum");
      frame = {};
      failures += require(!is_valid_can_channel_config({500000, false, 0, static_cast<CanMode>(value)}), "invalid mode enum");
    }
    frame.type = CanFrameType::remote;
    frame.length = 8;
    failures += require(is_valid_can_frame(frame), "valid classic remote frame");
    frame.bit_rate_switch = true;
    failures += require(!is_valid_can_frame(frame), "classic BRS rejected");
    frame.bit_rate_switch = false;
    frame.error_state_indicator = true;
    failures += require(!is_valid_can_frame(frame), "classic ESI rejected");
    frame = {};
    frame.identifier = 0x7FF;
    frame.length = 8;
    failures += require(is_valid_can_frame(frame), "classic boundaries");
    frame.identifier = 0x800;
    failures += require(!is_valid_can_frame(frame), "standard overflow");
    frame.identifier_format = CanIdentifierFormat::extended_29_bit;
    frame.identifier = 0x1FFFFFFF;
    failures += require(is_valid_can_frame(frame), "extended maximum");
    ++frame.identifier;
    failures += require(!is_valid_can_frame(frame), "extended overflow");
    frame.identifier = 0;
    frame.length = 9;
    failures += require(!is_valid_can_frame(frame), "classic length overflow");
    frame.format = CanFrameFormat::fd;
    for (unsigned length = 0; length <= 65; ++length) {
      frame.length = static_cast<std::uint8_t>(length);
      const bool valid = length <= 8 || length == 12 || length == 16 || length == 20 || length == 24 || length == 32 || length == 48 || length == 64;
      failures += require(is_valid_can_frame(frame) == valid, "FD length boundaries");
    }
    frame.length = 8;
    frame.type = CanFrameType::remote;
    failures += require(!is_valid_can_frame(frame), "FD remote rejected");
    frame = {};
    CanFilter filter{0x120, 0x7F0, true, false};
    frame.identifier = 0x12F;
    failures += require(filter.matches(frame), "masked standard match");
    frame.identifier = 0x130;
    failures += require(!filter.matches(frame), "masked mismatch");
    frame.identifier = 0x12F;
    frame.identifier_format = CanIdentifierFormat::extended_29_bit;
    failures += require(!filter.matches(frame), "standard excludes extended");
    filter.match_standard = false; filter.match_extended = true;
    failures += require(filter.matches(frame), "extended match");
    frame.identifier_format = CanIdentifierFormat::standard_11_bit;
    failures += require(!filter.matches(frame), "extended excludes standard");
    FakeCanDriver driver;
    CountingSink sink;
    CanBusRuntime bus{driver};
    for (const auto invalid : {CanFilter{0,0,false,false}, CanFilter{0x20000000,0,true,true}, CanFilter{0,0x20000000,true,true}, CanFilter{0,0x800,true,false}}) {
      failures += require(bus.subscribe(invalid,sink,kSinkExecution).status == CanSubscriptionStatus::invalid_argument && bus.subscription_count() == 0, "invalid filter consumes no slot");
    }
    for (std::size_t i = 0; i < CanBusRuntime::kMaxSubscriptions; ++i) {
      failures += require(bus.subscribe({},sink,kSinkExecution).handle.valid(), "subscription capacity slot");
    }
    failures += require(bus.subscribe({},sink,kSinkExecution).status == CanSubscriptionStatus::capacity_exhausted, "subscription capacity exhausted");
  }
  {
    FakeClock clock;
    safety::DeadlineWatchdog watchdog{clock};
    const auto maximum = std::numeric_limits<time::MonotonicTime::rep>::max();
    clock.uncertainty = time::MonotonicDuration{0};
    clock.set(time::MonotonicTime{maximum - 10});
    failures += require(watchdog.arm(std::chrono::nanoseconds{10}) == safety::DeadlineWatchdogStatus::ok && watchdog.deadline().count() == maximum, "representable maximum deadline");
    failures += require(watchdog.arm(std::chrono::nanoseconds{11}) == safety::DeadlineWatchdogStatus::invalid_argument && watchdog.deadline().count() == maximum && watchdog.state() == safety::DeadlineWatchdogState::armed, "overflow arm preserves state");
    clock.advance(std::chrono::nanoseconds{1});
    failures += require(watchdog.kick() == safety::DeadlineWatchdogStatus::expired, "overflow kick expires");
    watchdog.disarm();
    failures += require(watchdog.arm(std::chrono::nanoseconds{10}) == safety::DeadlineWatchdogStatus::invalid_argument && watchdog.state() == safety::DeadlineWatchdogState::disarmed, "overflow arm preserves disarmed");
  }
  {
    using namespace transport;
    FakeCanDriver driver;
    ReentrantSink callback;
    CountingSink observer;
    CanBusRuntime bus{driver};
    callback.bus = &bus;
    failures += require(bus.subscribe({},callback,kSinkExecution).handle.valid() && bus.subscribe({},observer,kSinkExecution).handle.valid() && bus.freeze_configuration(), "callback setup");
    const CanChannelConfig classic{500000,false,0,CanMode::normal,kTestClockDomain};
    failures += require(bus.start({}) == CanStatus::invalid_argument && driver.opens == 0, "config validated before open");
    failures += require(bus.start(classic) == CanStatus::ok, "classic start");
    CanFrame fd{}; fd.format = CanFrameFormat::fd;
    failures += require(bus.send(fd) == CanStatus::unsupported && driver.sends == 0, "classic channel rejects FD TX");
    CanFrame invalid{}; invalid.length = 9;
    failures += require(bus.send(invalid) == CanStatus::invalid_frame && driver.sends == 0, "invalid TX never reaches driver");
    failures += require(driver.inject({invalid,{}}) && bus.poll(1).status == CanStatus::invalid_frame && observer.count == 0, "invalid RX not delivered");
    failures += require(driver.inject({fd,{}}) && bus.poll(1).status == CanStatus::unsupported && observer.count == 0, "classic rejects FD RX");
    failures += require(driver.inject({}) && driver.inject({}), "callback RX setup");
    const auto reads = driver.receives;
    const auto result = bus.poll(8);
    failures += require(callback.rejected && result.deliveries == 2 && observer.count == 1 && driver.receives == reads + 1 && bus.state() == CanBusState::stopped, "reentrancy rejected and stop deferred before next RX");
    failures += require(bus.start({500000,false,0,CanMode::listen_only,kTestClockDomain}) == CanStatus::ok && bus.send({}) == CanStatus::unsupported && driver.sends == 0, "restart and listen-only TX enforcement");
    bus.stop();
    failures += require(bus.start(classic) == CanStatus::ok, "restart preserves subscriptions");
    failures += require(bus.poll(1).deliveries == 0 && observer.count == 1,
                        "restart does not leak RX from the previous closed session");
  }
  for (const auto fatal : {transport::CanStatus::bus_off, transport::CanStatus::io_error, transport::CanStatus::not_open}) {
    for (unsigned path = 0; path < 3; ++path) {
      using namespace transport;
      FakeCanDriver driver;
      CanBusRuntime bus{driver};
      const CanChannelConfig config{500000,false,0,CanMode::normal,kTestClockDomain};
      failures += require(bus.freeze_configuration(), "fault setup freeze");
      if (path == 0) { driver.open_status = fatal; }
      const auto started = bus.start(config);
      if (path != 0) {
        failures += require(started == CanStatus::ok, "fault setup start");
        if (path == 1) { driver.send_status = fatal; failures += require(bus.send({}) == fatal, "fatal TX status"); }
        else { driver.receive_status = fatal; failures += require(bus.poll(1).status == fatal, "fatal RX status"); }
      } else { failures += require(started == fatal, "fatal open status"); }
      const auto operations = driver.opens + driver.closes + driver.sends + driver.receives;
      failures += require(bus.state() == CanBusState::faulted &&
          bus.send({}) == CanStatus::faulted &&
          bus.poll(1).status == CanStatus::faulted &&
          bus.start(config) == CanStatus::faulted, "fault latches");
      bus.stop();
      failures += require(bus.state() == CanBusState::faulted && operations == driver.opens + driver.closes + driver.sends + driver.receives, "fault blocks driver operations including stop");
      driver.open_status = driver.send_status = driver.receive_status = CanStatus::ok;
      const auto closes_before_recovery = driver.closes;
      failures += require(bus.recover() == CanStatus::ok && driver.closes == closes_before_recovery + (path == 0 ? 0U : 1U) && !driver.is_open() && bus.state() == CanBusState::stopped && bus.start(config) == CanStatus::ok, "explicit recovery closes only owned driver then restart");
      bus.stop();
    }
  }


  {
    using namespace safety;
    FakeClock clock;
    DeadlineWatchdog watchdog{clock};
    failures += require(watchdog.arm(std::chrono::nanoseconds{10}) ==
                            DeadlineWatchdogStatus::ok, "initial arm");
    clock.advance(std::chrono::nanoseconds{4});
    failures += require(watchdog.kick() == DeadlineWatchdogStatus::ok &&
                            watchdog.deadline().count() == 13, "kick extends conservative deadline");
    clock.set(time::MonotonicTime{10});
    failures += require(watchdog.poll() == DeadlineWatchdogStatus::ok,
                        "old deadline no longer expires");
    failures += require(watchdog.arm(std::chrono::nanoseconds{20}) ==
                            DeadlineWatchdogStatus::ok && watchdog.deadline().count() == 29,
                        "arm replaces armed deadline conservatively");
    for (const auto state : {DeadlineWatchdogState::armed, DeadlineWatchdogState::expired}) {
      if (state == DeadlineWatchdogState::expired) {
        clock.set(watchdog.deadline());
        failures += require(watchdog.poll() == DeadlineWatchdogStatus::expired,
                            "exact deadline equality expires");
      }
      const auto deadline = watchdog.deadline();
      for (const auto timeout : {0, -1}) {
        failures += require(watchdog.arm(std::chrono::nanoseconds{timeout}) ==
                                DeadlineWatchdogStatus::invalid_argument &&
                                watchdog.state() == state && watchdog.deadline() == deadline,
                            "invalid arm preserves state and deadline");
      }
    }
    failures += require(watchdog.arm(std::chrono::nanoseconds{5}) ==
                            DeadlineWatchdogStatus::ok &&
                            watchdog.state() == DeadlineWatchdogState::armed &&
                            watchdog.deadline().count() == 33, "explicit conservative rearm from expired");
    clock.set(watchdog.deadline());
    failures += require(watchdog.kick() == DeadlineWatchdogStatus::expired,
                        "kick at exact deadline expires");
    watchdog.disarm();
    failures += require(watchdog.poll() == DeadlineWatchdogStatus::not_armed && watchdog.kick() == DeadlineWatchdogStatus::not_armed, "disarmed normal operations");
    failures += require(watchdog.arm(std::chrono::nanoseconds{0}) ==
                            DeadlineWatchdogStatus::invalid_argument &&
                            watchdog.state() == DeadlineWatchdogState::disarmed &&
                            watchdog.deadline().count() == 0, "invalid disarmed arm unchanged");
    failures += require(watchdog.arm(std::chrono::nanoseconds{10}) == DeadlineWatchdogStatus::ok &&
                            watchdog.poll() == DeadlineWatchdogStatus::ok &&
                            watchdog.kick() == DeadlineWatchdogStatus::ok, "normal rearm after disarm");
  }
  {
    using namespace transport;
    const CanChannelConfig classic{500000, false, 0, CanMode::normal, kTestClockDomain};
    const CanChannelConfig fd{500000, true, 2000000, CanMode::normal, kTestClockDomain};
    for (unsigned missing = 0; missing < 3; ++missing) {
      FakeCanDriver driver;
      if (missing == 0) { driver.capabilities_value.can_fd = false; }
      if (missing == 1) { driver.capabilities_value.listen_only = false; }
      if (missing == 2) { driver.capabilities_value.classic_can = false; }
      CanBusRuntime bus{driver};
      failures += require(bus.freeze_configuration(), "capability setup freeze");
      auto config = missing == 0 ? fd : classic;
      if (missing == 1) { config.mode = CanMode::listen_only; }
      failures += require(bus.start(config) == CanStatus::unsupported && driver.opens == 0 &&
                              bus.state() == CanBusState::ready,
                          "unsupported capability rejected before open");
    }
    for (unsigned limited = 0; limited < 2; ++limited) {
      FakeCanDriver driver;
      driver.capabilities_value.bit_rate_switch = false;
      if (limited == 1) { driver.capabilities_value.max_payload_bytes = 8; }
      CanBusRuntime bus{driver};
      failures += require(bus.freeze_configuration() && bus.start(fd) == CanStatus::ok,
                          "limited FD setup");
      CanFrame frame{};
      frame.format = CanFrameFormat::fd;
      frame.bit_rate_switch = limited == 0;
      frame.length = limited == 0 ? 8 : 12;
      failures += require(bus.send(frame) == CanStatus::unsupported && driver.sends == 0,
                          "unsupported BRS or payload rejected before send");
    }
    FakeCanDriver driver;
    {
      CanBusRuntime bus{driver};
      driver.open_status = CanStatus::unsupported;
      failures += require(bus.freeze_configuration() && bus.start(classic) == CanStatus::unsupported &&
                              bus.state() == CanBusState::ready, "nonfatal open retryable");
      driver.open_status = CanStatus::ok;
      failures += require(bus.start(classic) == CanStatus::ok, "retry open succeeds");
    }
    failures += require(!driver.is_open() && driver.closes == 1,
                        "running destructor closes physical channel");
    for (const auto status : {CanStatus::ok, CanStatus::unsupported, CanStatus::io_error}) {
      const auto closes = driver.closes;
      {
        CanBusRuntime replacement{driver};
        driver.open_status = status;
        failures += require(replacement.freeze_configuration() && replacement.start(classic) == status,
                            "replacement starts or reports injected open failure");
        if (status == CanStatus::io_error) {
          failures += require(replacement.state() == CanBusState::faulted, "failed open faulted");
        }
      }
      failures += require(driver.closes == closes + (status == CanStatus::ok ? 1U : 0U) && !driver.is_open() && driver.partial_resources == 0,
                          "destructor closes only successfully acquired driver");
      driver.open_status = CanStatus::ok;
      CanBusRuntime replacement{driver};
      failures += require(replacement.freeze_configuration() && replacement.start(classic) == CanStatus::ok,
                          "replacement after cleanup starts");
    }
  }
  {
    using namespace transport;
    const CanChannelConfig config{500000,false,0,CanMode::normal,kTestClockDomain};
    FakeCanDriver driver;
    {
      CanBusRuntime owner{driver};
      failures += require(owner.freeze_configuration() && owner.start(config) == CanStatus::ok, "owner start");
      {
        CanBusRuntime observer{driver};
        failures += require(observer.freeze_configuration() && observer.start({}) == CanStatus::invalid_argument, "second runtime never acquires on rejected configuration");
        observer.stop();
        failures += require(observer.recover() == CanStatus::invalid_state, "unowned recovery rejected");
      }
      failures += require(driver.is_open() && driver.closes == 0 && owner.send({}) == CanStatus::ok,
                          "unacquired second runtime cannot close owner channel");
      const auto releases_before_stop = driver.lease_releases;
      owner.stop(); owner.stop();
      failures += require(
          driver.closes == 1 &&
              driver.lease_releases == releases_before_stop,
          "stop closes one session but retains physical-channel lease");
    }
    failures += require(
        driver.closes == 1 && driver.lease_releases == 1,
        "stopped destructor releases retained lease without closing again");
    {
      FakeCanDriver shared_driver;
      CanBusRuntime b{shared_driver};
      failures += require(
          b.freeze_configuration(),
          "second runtime configured before owner lifetime test");
      const auto opens_before = shared_driver.opens;
      {
        CanBusRuntime a{shared_driver};
        failures += require(
            a.freeze_configuration() &&
                a.start(config) == CanStatus::ok,
            "shared driver owner setup");
        const auto opens = shared_driver.opens;
        const auto closes = shared_driver.closes;
        const auto resources = shared_driver.partial_resources;
        failures += require(
            b.start(config) == CanStatus::busy &&
                b.state() == CanBusState::ready &&
                shared_driver.opens == opens &&
                shared_driver.closes == closes &&
                shared_driver.partial_resources == resources &&
                shared_driver.is_open() &&
                a.send({}) == CanStatus::ok,
            "second runtime cannot acquire an owned physical channel");

        a.stop();
        failures += require(
            !shared_driver.is_open() &&
                b.start(config) == CanStatus::busy &&
                a.start(config) == CanStatus::ok &&
                shared_driver.opens == opens + 1U,
            "stopped owner retains lease and can restart its session");
        a.stop();
      }

      failures += require(
          b.start(config) == CanStatus::ok &&
              b.send({}) == CanStatus::ok &&
              shared_driver.opens == opens_before + 3U,
          "second runtime acquires only after former owner destruction");
    }
    {
      FakeCanDriver leased_driver;
      CanBusRuntime a{leased_driver};
      CanBusRuntime b{leased_driver};
      failures += require(a.freeze_configuration() && b.freeze_configuration() &&
                              a.start(config) == CanStatus::ok,
                          "faulted lease setup");
      leased_driver.send_status = CanStatus::not_open;
      leased_driver.close_on_send_failure = true;
      failures += require(a.send({}) == CanStatus::not_open &&
                              a.state() == CanBusState::faulted &&
                              !leased_driver.is_open(),
                          "physical close latches fault while lease remains owned");
      const auto opens_before_blocked_start = leased_driver.opens;
      const auto releases_before_recovery = leased_driver.lease_releases;
      a.stop();
      failures += require(b.start(config) == CanStatus::busy &&
                              leased_driver.opens == opens_before_blocked_start &&
                              leased_driver.lease_releases == releases_before_recovery,
                          "faulted runtime blocks second owner even when physical channel is closed");
      leased_driver.send_status = CanStatus::ok;
      failures += require(a.recover() == CanStatus::ok &&
                              leased_driver.lease_releases == releases_before_recovery + 1U &&
                              b.start(config) == CanStatus::ok &&
                              b.send({}) == CanStatus::ok,
                          "recovery releases lease and second runtime can acquire");
      a.stop();
      failures += require(b.send({}) == CanStatus::ok,
                          "recovered former owner cannot close new owner channel");
    }
    for (const auto status : {CanStatus::busy, CanStatus::would_block, CanStatus::unsupported,
                             CanStatus::invalid_argument, CanStatus::already_open, CanStatus::io_error}) {
      const auto closes = driver.closes;
      const auto lease_acquires = driver.lease_acquires;
      const auto lease_releases = driver.lease_releases;
      {
        CanBusRuntime bus{driver};
        driver.open_status = status;
        failures += require(bus.freeze_configuration() && bus.start(config) == status &&
                                !driver.is_open() && driver.partial_resources == 0 &&
                                driver.lease_acquires == lease_acquires + 1U &&
                                driver.lease_releases == lease_releases + 1U,
                            "failed open rolls back resources and provisional lease");
        if (status == CanStatus::io_error) {
          failures += require(bus.recover() == CanStatus::ok, "failed-open recovery");
        }
        bus.stop();
      }
      failures += require(driver.closes == closes, "failed open grants no cleanup ownership");
    }
    driver.open_status = CanStatus::ok;
    {
      CanBusRuntime bus{driver};
      failures += require(bus.freeze_configuration() && bus.start(config) == CanStatus::ok, "fault destructor start");
      driver.send_status = CanStatus::io_error;
      failures += require(bus.send({}) == CanStatus::io_error, "fault destructor latch");
    }
    failures += require(driver.closes == 2 && !driver.is_open(), "faulted owner destructor closes");
  }
  {
    using namespace transport;
    for (unsigned bits = 0; bits < 16; ++bits) {
      for (const bool fd : {false, true}) {
        for (const bool listen : {false, true}) {
          FakeCanDriver driver;
          driver.capabilities_value = {bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8), 64};
          CanBusRuntime bus{driver};
          failures += require(bus.freeze_configuration(), "capability combination freeze");
          const bool supported = (bits & 1) && (!fd || (bits & 2)) && (!listen || (bits & 8));
          const auto result = bus.start({500000, fd, fd ? 2000000U : 0U,
                             listen ? CanMode::listen_only : CanMode::normal,
                             kTestClockDomain});
          failures += require(result == (supported ? CanStatus::ok : CanStatus::unsupported) &&
                                  driver.opens == (supported ? 1U : 0U), "all channel capability combinations");
        }
      }
    }
  }
  {
    FakeClock clock;
    safety::DeadlineWatchdog watchdog{clock};
    failures += require(
        time::is_valid_clock_properties(watchdog.clock_properties()),
        "watchdog requires healthy continuous clock properties");
    failures += require(
        watchdog.arm(time::MonotonicDuration{10}) ==
            safety::DeadlineWatchdogStatus::ok,
        "watchdog healthy clock arm");
    clock.status = time::MonotonicClockStatus::unavailable;
    failures += require(
        watchdog.poll() == safety::DeadlineWatchdogStatus::clock_fault &&
            watchdog.state() == safety::DeadlineWatchdogState::expired,
        "armed watchdog fails closed on unavailable clock");
  }
  {
    FakeClock clock;
    clock.properties_value.continuous_across_suspend = false;
    safety::DeadlineWatchdog watchdog{clock};
    failures += require(
        watchdog.arm(time::MonotonicDuration{10}) ==
                safety::DeadlineWatchdogStatus::clock_fault &&
            watchdog.state() == safety::DeadlineWatchdogState::disarmed,
        "watchdog rejects clock that does not advance across suspend");
  }
  {
    FakeClock clock;
    safety::DeadlineWatchdog watchdog{clock};
    failures += require(
        watchdog.arm(time::MonotonicDuration{2}) ==
                safety::DeadlineWatchdogStatus::insufficient_clock_precision &&
            watchdog.state() == safety::DeadlineWatchdogState::disarmed &&
            watchdog.deadline().count() == 0,
        "watchdog rejects timeout inside declared clock ambiguity");
    failures += require(
        watchdog.arm(time::MonotonicDuration{10}) ==
                safety::DeadlineWatchdogStatus::ok,
        "watchdog precision setup arm");
    const auto deadline = watchdog.deadline();
    failures += require(
        watchdog.arm(time::MonotonicDuration{2}) ==
                safety::DeadlineWatchdogStatus::insufficient_clock_precision &&
            watchdog.state() == safety::DeadlineWatchdogState::armed &&
            watchdog.deadline() == deadline,
        "insufficient precision preserves existing watchdog state");
  }
  {
    FakeClock clock;
    clock.set(time::MonotonicTime{100});
    safety::DeadlineWatchdog watchdog{clock};
    failures += require(
        watchdog.arm(time::MonotonicDuration{10}) ==
                safety::DeadlineWatchdogStatus::ok &&
            watchdog.deadline().count() == 109,
        "watchdog deadline uses conservative arm lower bound");
    clock.set(time::MonotonicTime{108});
    failures += require(
        watchdog.poll() == safety::DeadlineWatchdogStatus::expired &&
            watchdog.state() == safety::DeadlineWatchdogState::expired,
        "watchdog expiry uses conservative poll upper bound");
  }
  {
    FakeClock clock;
    clock.uncertainty = time::MonotonicDuration{2};
    safety::DeadlineWatchdog watchdog{clock};
    failures += require(
        watchdog.arm(time::MonotonicDuration{10}) ==
                safety::DeadlineWatchdogStatus::clock_fault &&
            watchdog.state() == safety::DeadlineWatchdogState::disarmed,
        "watchdog rejects reading uncertainty above declared maximum");
  }
  {
    FakeClock clock;
    clock.set(time::MonotonicTime{100});
    safety::DeadlineWatchdog watchdog{clock};
    failures += require(
        watchdog.arm(time::MonotonicDuration{50}) ==
            safety::DeadlineWatchdogStatus::ok,
        "watchdog backward-clock setup");
    clock.set(time::MonotonicTime{99});
    failures += require(
        watchdog.poll() == safety::DeadlineWatchdogStatus::clock_fault &&
            watchdog.state() == safety::DeadlineWatchdogState::expired,
        "watchdog detects backward clock discontinuity");
  }
  {
    FakeClock clock;
    safety::DeadlineWatchdog watchdog{clock};
    failures += require(
        watchdog.arm(time::MonotonicDuration{50}) ==
            safety::DeadlineWatchdogStatus::ok,
        "watchdog domain setup");
    clock.reading_domain = time::MonotonicClockDomainId{0xBEEFU};
    failures += require(
        watchdog.kick() == safety::DeadlineWatchdogStatus::clock_fault &&
            watchdog.state() == safety::DeadlineWatchdogState::expired,
        "watchdog rejects wrong clock domain");
  }
  {
    using namespace transport;
    FakeCanDriver driver;
    CountingSink first;
    CountingSink second;
    CanBusRuntime bus{driver};
    failures += require(
        bus.subscribe({}, first, {time::MonotonicDuration{0}}).status ==
            CanSubscriptionStatus::invalid_argument,
        "zero callback budget rejected");
    failures += require(
        bus.subscribe({}, first, kSinkExecution).handle.valid() &&
            bus.subscribe({}, second, kSinkExecution).handle.valid() &&
            bus.freeze_configuration(),
        "execution-budget subscriptions");
    const CanChannelConfig config{
        500000, false, 0, CanMode::normal, kTestClockDomain};
    failures += require(
        bus.start(config) == CanStatus::ok,
        "execution-budget runtime start");
    const auto poll_budget = bus.max_poll_execution_duration(3U);
    const auto send_budget = bus.max_send_execution_duration();
    failures += require(
        bus.max_single_frame_dispatch_duration().count() == 200 &&
            send_budget.status == CanStatus::ok &&
            send_budget.max_duration.count() == 6000 &&
            poll_budget.status == CanStatus::ok &&
            poll_budget.max_duration.count() == 8600,
        "declared external-work budgets include handled close paths");
    const auto receives_before_oversize = driver.receives;
    failures += require(
        bus.poll((std::numeric_limits<std::size_t>::max)()).status ==
                CanStatus::invalid_argument &&
            driver.receives == receives_before_oversize,
        "oversized poll rejected before Win32 delivery counter can overflow");
  }
  {
    using namespace transport;
    FakeCanDriver driver;
    driver.execution_contract_value.max_try_receive_duration =
        time::MonotonicDuration{0};
    CanBusRuntime bus{driver};
    failures += require(
        bus.freeze_configuration() &&
            bus.start(
                {500000, false, 0, CanMode::normal, kTestClockDomain}) ==
                CanStatus::contract_violation &&
            bus.state() == CanBusState::faulted &&
            driver.opens == 0U,
        "invalid driver execution contract fails closed before open");
    failures += require(
        bus.recover() == CanStatus::ok,
        "contract-fault recovery without acquired channel");
  }
  {
    using namespace transport;
    FakeCanArbiter shared_arbiter;
    FakeCanDriver first_driver;
    FakeCanDriver second_driver;
    first_driver.arbiter_override = &shared_arbiter;
    second_driver.arbiter_override = &shared_arbiter;
    first_driver.physical_channel_id_value = {0xCA01U};
    second_driver.physical_channel_id_value = {0xCA01U};
    CanBusRuntime first{first_driver};
    CanBusRuntime second{second_driver};
    const CanChannelConfig config{
        500000, false, 0, CanMode::normal, kTestClockDomain};
    failures += require(
        first.freeze_configuration() &&
            second.freeze_configuration() &&
            first.start(config) == CanStatus::ok &&
            second.start(config) == CanStatus::busy,
        "separate wrappers share one physical-channel arbiter");
    first_driver.send_status = CanStatus::not_open;
    first_driver.close_on_send_failure = true;
    failures += require(
        first.send({}) == CanStatus::not_open &&
            first.state() == CanBusState::faulted &&
            second.start(config) == CanStatus::busy,
        "faulted wrapper retains physical-channel lease");
    first_driver.send_status = CanStatus::ok;
    failures += require(
        first.recover() == CanStatus::ok &&
            second.start(config) == CanStatus::ok,
        "physical-channel lease transfers only after recovery");
  }
  {
    using namespace transport;
    FakeCanDriver driver;
    CountingSink sink;
    CanBusRuntime bus{driver};
    const CanChannelConfig config{
        500000, false, 0, CanMode::normal, kTestClockDomain};
    failures += require(
        bus.subscribe({}, sink, kSinkExecution).handle.valid() &&
            bus.freeze_configuration() &&
            bus.start(config) == CanStatus::ok,
        "timestamp-domain setup");
    driver.auto_timestamp = false;
    ReceivedCanFrame wrong_domain{};
    wrong_domain.timestamp = {
        time::MonotonicClockStatus::ok,
        time::MonotonicClockDomainId{0xDEADU},
        time::MonotonicTime{1},
        time::MonotonicDuration{1}};
    failures += require(
        driver.inject(wrong_domain) &&
            bus.poll(1).status == CanStatus::invalid_timestamp &&
            bus.state() == CanBusState::faulted &&
            sink.count == 0U,
        "wrong-domain RX timestamp faults before delivery");
    failures += require(
        bus.recover() == CanStatus::ok,
        "timestamp fault recovery");
    failures += require(
        bus.start(config) == CanStatus::ok,
        "timestamp recovery restart");
    ReceivedCanFrame later{};
    later.timestamp = {
        time::MonotonicClockStatus::ok,
        kTestClockDomain,
        time::MonotonicTime{10},
        time::MonotonicDuration{1}};
    ReceivedCanFrame earlier{};
    earlier.timestamp = {
        time::MonotonicClockStatus::ok,
        kTestClockDomain,
        time::MonotonicTime{9},
        time::MonotonicDuration{1}};
    failures += require(
        driver.inject(later) && driver.inject(earlier) &&
            bus.poll(1).status == CanStatus::ok &&
            bus.poll(1).status == CanStatus::invalid_timestamp &&
            bus.state() == CanBusState::faulted,
        "backward RX timestamp faults within one transport session");
    failures += require(
        bus.recover() == CanStatus::ok &&
            bus.start(config) == CanStatus::ok,
        "timestamp ordering recovery restart");
    ReceivedCanFrame excessive_uncertainty{};
    excessive_uncertainty.timestamp = {
        time::MonotonicClockStatus::ok,
        kTestClockDomain,
        time::MonotonicTime{2},
        time::MonotonicDuration{101}};
    failures += require(
        driver.inject(excessive_uncertainty) &&
            bus.poll(1).status == CanStatus::invalid_timestamp &&
            bus.state() == CanBusState::faulted,
        "RX timestamp uncertainty bound enforced");
    failures += require(
        bus.recover() == CanStatus::ok,
        "timestamp uncertainty recovery");
    driver.auto_timestamp = true;
    failures += require(
        bus.start(config) == CanStatus::ok,
        "timestamp loss-observation restart");
    driver.rx_dropped_report = 7U;
    const auto loss = bus.poll(1);
    failures += require(
        loss.status == CanStatus::ok &&
            loss.rx_dropped == 7U &&
            loss.frames_received == 0U,
        "RX loss is observable even when queue is empty");
  }
  {
    using namespace transport;
    const CanChannelConfig config{
        500000, false, 0, CanMode::normal, kTestClockDomain};

    for (unsigned path = 0U; path < 3U; ++path) {
      FakeCanDriver driver;
      CanBusRuntime bus{driver};
      driver.reentrant_bus = &bus;
      failures += require(
          bus.freeze_configuration(),
          "driver reentrancy setup");

      if (path == 0U) {
        driver.reenter_stop_on_open = true;
        failures += require(
            bus.start(config) == CanStatus::contract_violation &&
                bus.state() == CanBusState::faulted,
            "driver stop reentry during open fails closed");
      } else {
        failures += require(
            bus.start(config) == CanStatus::ok,
            "driver reentrancy running setup");
        if (path == 1U) {
          driver.reenter_stop_on_send = true;
          failures += require(
              bus.send({}) == CanStatus::contract_violation &&
                  bus.state() == CanBusState::faulted,
              "driver stop reentry during send fails closed");
        } else {
          driver.reenter_stop_on_receive = true;
          failures += require(
              bus.poll(1).status == CanStatus::contract_violation &&
                  bus.state() == CanBusState::faulted,
              "driver stop reentry during receive fails closed");
        }
      }

      failures += require(
          bus.recover() == CanStatus::ok,
          "driver reentrancy explicit recovery");
    }
  }
  {
    using namespace transport;
    const CanChannelConfig config{
        500000, false, 0, CanMode::normal, kTestClockDomain};
    FakeCanDriver driver;
    CanBusRuntime owner{driver};
    CanBusRuntime contender{driver};
    driver.reentrant_bus = &owner;
    failures += require(
        owner.freeze_configuration() &&
            contender.freeze_configuration() &&
            owner.start(config) == CanStatus::ok,
        "close-reentrancy ownership setup");
    const auto releases_before_close = driver.lease_releases;
    driver.reenter_stop_on_close = true;
    owner.stop();
    failures += require(
        owner.state() == CanBusState::faulted &&
            driver.lease_releases == releases_before_close &&
            contender.start(config) == CanStatus::busy,
        "close reentrancy faults while retaining physical-channel lease");
    driver.reenter_stop_on_close = false;
    failures += require(
        owner.recover() == CanStatus::ok &&
            contender.start(config) == CanStatus::ok,
        "explicit recovery releases lease after close reentrancy");
  }
  {
    using namespace transport;
    const CanChannelConfig config{
        500000, false, 0, CanMode::normal, kTestClockDomain};
    FakeCanDriver driver;
    ReentrantSink callback;
    CanBusRuntime bus{driver};
    callback.bus = &bus;
    driver.reentrant_bus = &bus;
    driver.reenter_stop_on_close = true;
    failures += require(
        bus.subscribe({}, callback, kSinkExecution).handle.valid() &&
            bus.freeze_configuration() &&
            bus.start(config) == CanStatus::ok &&
            driver.inject({}),
        "deferred-close reentrancy setup");
    const auto result = bus.poll(1U);
    failures += require(
        callback.rejected &&
            result.status == CanStatus::contract_violation &&
            bus.state() == CanBusState::faulted,
        "poll reports contract violation when deferred close reenters runtime");
    driver.reenter_stop_on_close = false;
    failures += require(
        bus.recover() == CanStatus::ok,
        "deferred-close reentrancy recovery");
  }
  {
    using namespace transport;
    const CanChannelConfig config{
        500000, false, 0, CanMode::normal, kTestClockDomain};
    FakeCanDriver driver;
    CanBusRuntime bus{driver};
    driver.reentrant_bus = &bus;
    failures += require(
        bus.freeze_configuration() &&
            bus.start(config) == CanStatus::ok,
        "arbiter-release reentrancy setup");
    driver.send_status = CanStatus::io_error;
    failures += require(
        bus.send({}) == CanStatus::io_error &&
            bus.state() == CanBusState::faulted,
        "arbiter-release reentrancy fault setup");
    driver.reenter_stop_on_release = true;
    failures += require(
        bus.recover() == CanStatus::contract_violation &&
            bus.state() == CanBusState::faulted,
        "arbiter release reentrancy is detected during explicit recovery");
    driver.reenter_stop_on_release = false;
    failures += require(
        bus.recover() == CanStatus::ok,
        "recovery can complete after arbiter contract violation is removed");
  }
  {
    using namespace transport;
    FakeCanDriver driver;
    CountingSink sink;
    FakeClock clock;
    safety::DeadlineWatchdog watchdog{clock};
    CanBusRuntime bus{driver};
    failures += require(bus.subscribe({}, sink, kSinkExecution).handle.valid() && bus.freeze_configuration() &&
                            driver.inject({}), "allocation probe setup");
    // Verify the probe itself before measuring Core calls.
    const auto probe_before = allocation_probe::count;
    void* probe = ::operator new(1);
    const auto probe_after = allocation_probe::count;
    // Indirect deallocation keeps the replacement allocation functions opaque
    // to compiler allocation-pair heuristics in optimized builds.
    void (*volatile release_probe)(void*) = ::operator delete;
    release_probe(probe);
    failures += require(probe_after == probe_before + 1, "allocation counter active");
    FakeCanDriver cleanup_driver;
    const auto before = allocation_probe::count;
    const auto started = bus.start({500000, false, 0, CanMode::normal, kTestClockDomain});
    const auto sent = bus.send({});
    const auto received = bus.poll(8);
    const auto armed = watchdog.arm(std::chrono::nanoseconds{10});
    const auto kicked = watchdog.kick();
    const auto polled = watchdog.poll();
    driver.send_status = CanStatus::io_error;
    const auto fault = bus.send({});
    const auto recovered = bus.recover();
    const auto restarted = bus.start({500000, false, 0, CanMode::normal, kTestClockDomain});
    bus.stop();
    watchdog.disarm();
    {
      CanBusRuntime cleanup_bus{cleanup_driver};
      const auto frozen = cleanup_bus.freeze_configuration();
      const auto cleanup_start = cleanup_bus.start({500000, false, 0, CanMode::normal, kTestClockDomain});
      if (!frozen || cleanup_start != CanStatus::ok) { ++failures; }
    }
    const auto after = allocation_probe::count;
    failures += require(after == before, "Core runtime operations allocate no heap memory");
    failures += require(started == CanStatus::ok && sent == CanStatus::ok &&
                            received.deliveries == 1 && sink.count == 1 &&
                            armed == safety::DeadlineWatchdogStatus::ok &&
                            kicked == safety::DeadlineWatchdogStatus::ok &&
                            polled == safety::DeadlineWatchdogStatus::ok &&
                            fault == CanStatus::io_error && recovered == CanStatus::ok &&
                            restarted == CanStatus::ok, "allocation probe operations succeed");
  }

  {
    using namespace domain;

    const auto actuator_capabilities =
        dut_capability_mask(DutCapability::raw_can) |
        dut_capability_mask(DutCapability::cyclic_can) |
        dut_capability_mask(DutCapability::active_control) |
        dut_capability_mask(DutCapability::feedback);
    const DutDescriptor actuator{
        1U,
        DutClass::actuator,
        kAllSupported,
        actuator_capabilities};

    failures += require(
        is_valid_dut_descriptor(actuator) &&
            has_dut_capability(
                actuator.capabilities,
                DutCapability::raw_can) &&
            has_dut_capability(
                actuator.capabilities,
                DutCapability::cyclic_can) &&
            !has_dut_capability(
                actuator.capabilities,
                DutCapability::uds),
        "DUT contract supports raw cyclic actuator without diagnostics");

    const DutDescriptor ecu{
        2U,
        DutClass::ecu,
        kTruck,
        static_cast<DutCapabilityMask>(
            dut_capability_mask(DutCapability::raw_can) |
            dut_capability_mask(DutCapability::isotp) |
            dut_capability_mask(DutCapability::uds) |
            dut_capability_mask(
                DutCapability::requires_environment))};

    failures += require(
        is_valid_dut_descriptor(ecu) &&
            has_dut_capability(
                ecu.capabilities,
                DutCapability::uds) &&
            has_dut_capability(
                ecu.capabilities,
                DutCapability::requires_environment),
        "DUT contract supports ECU diagnostics plus bench environment");

    const DutDescriptor invalid_cyclic{
        3U,
        DutClass::actuator,
        kTruck,
        dut_capability_mask(DutCapability::cyclic_can)};
    failures += require(
        !is_valid_dut_descriptor(invalid_cyclic),
        "cyclic CAN capability requires raw CAN capability");

    const DutDescriptor invalid_uds{
        4U,
        DutClass::ecu,
        kTruck,
        dut_capability_mask(DutCapability::uds)};
    failures += require(
        !is_valid_dut_descriptor(invalid_uds),
        "UDS capability requires ISO-TP or DoIP transport");

    const DutDescriptor invalid_sensor_control{
        5U,
        DutClass::sensor,
        kAgri,
        dut_capability_mask(DutCapability::active_control)};
    failures += require(
        !is_valid_dut_descriptor(invalid_sensor_control),
        "passive sensor DUT cannot declare active-control capability");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_FOUNDATION_TESTS=PASS\n";
  }

  return failures == 0 ? 0 : 1;
}
