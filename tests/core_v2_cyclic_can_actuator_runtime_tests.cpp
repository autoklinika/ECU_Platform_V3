
#include "ecu/core_v2/actuation/cyclic_can_actuator_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;

constexpr time::MonotonicClockDomainId kClockDomain{0xAC710U};

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

class FakeClock final : public time::IMonotonicClock {
 public:
  [[nodiscard]] time::MonotonicClockProperties properties()
      const noexcept override {
    return {
        kClockDomain,
        time::MonotonicDuration{100},
        time::MonotonicDuration{1000},
        time::MonotonicDuration{100},
        true};
  }

  [[nodiscard]] time::MonotonicClockReading read()
      const noexcept override {
    return {
        status,
        kClockDomain,
        now,
        uncertainty};
  }

  void set_ns(const std::int64_t value) noexcept {
    now = time::MonotonicTime{value};
  }

  time::MonotonicClockStatus status{
      time::MonotonicClockStatus::ok};
  time::MonotonicTime now{0};
  time::MonotonicDuration uncertainty{0};
};

class FakeCanDriver final : public transport::ICanDriver,
                            public transport::ICanChannelArbiter {
 public:
  [[nodiscard]] transport::CanPhysicalChannelId
  physical_channel_id() const noexcept override {
    return {1U};
  }

  [[nodiscard]] transport::ICanChannelArbiter&
  channel_arbiter() noexcept override {
    return *this;
  }

  [[nodiscard]] transport::CanDriverExecutionContract
  execution_contract() const noexcept override {
    return {
        time::MonotonicDuration{1000},
        time::MonotonicDuration{1000},
        time::MonotonicDuration{5000},
        time::MonotonicDuration{5000},
        time::MonotonicDuration{2000},
        time::MonotonicDuration{2000},
        time::MonotonicDuration{100}};
  }

  [[nodiscard]] transport::CanCapabilities capabilities()
      const noexcept override {
    return {
        true,
        true,
        true,
        true,
        64U};
  }

  [[nodiscard]] bool try_acquire(
      const transport::CanPhysicalChannelId channel,
      const void* owner_token) noexcept override {
    if (channel.value != 1U || owner_token == nullptr) {
      return false;
    }
    if (lease_owner_ != nullptr && lease_owner_ != owner_token) {
      return false;
    }
    lease_owner_ = owner_token;
    return true;
  }

  void release(
      const transport::CanPhysicalChannelId channel,
      const void* owner_token) noexcept override {
    if (channel.value == 1U && lease_owner_ == owner_token) {
      lease_owner_ = nullptr;
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
    open_ = true;
    return transport::CanStatus::ok;
  }

  void close() noexcept override {
    open_ = false;
  }

  [[nodiscard]] bool is_open() const noexcept override {
    return open_;
  }

  [[nodiscard]] transport::CanStatus try_send(
      const transport::CanFrame& frame) noexcept override {
    if (!open_) {
      return transport::CanStatus::not_open;
    }

    const auto status = next_send_status;
    next_send_status = transport::CanStatus::ok;
    if (status != transport::CanStatus::ok) {
      return status;
    }

    if (sent_count >= sent.size()) {
      return transport::CanStatus::would_block;
    }

    sent[sent_count] = frame;
    ++sent_count;
    return transport::CanStatus::ok;
  }

  [[nodiscard]] transport::CanReceiveResult
  try_receive() noexcept override {
    return {transport::CanStatus::would_block, {}, 0U};
  }

  void clear_sent() noexcept {
    sent_count = 0U;
    sent = {};
  }

  std::array<transport::CanFrame, 128U> sent{};
  std::size_t sent_count{0U};
  transport::CanStatus next_send_status{
      transport::CanStatus::ok};

 private:
  const void* lease_owner_{nullptr};
  bool open_{false};
};

class TestProgram final
    : public actuation::ICyclicCanActuatorProgram {
 public:
  [[nodiscard]] actuation::CyclicCanActuatorProgramContract
  execution_contract() const noexcept override {
    return contract;
  }

  [[nodiscard]] actuation::ActuatorProgramStatus
  render_active_cycle(
      const std::uint32_t cycle_sequence,
      actuation::CyclicCanFrameBatch& batch) noexcept override {
    ++active_render_calls;
    batch = {};

    if (fault_active) {
      return actuation::ActuatorProgramStatus::fault;
    }

    if (oversized_active) {
      batch.count = 2U;
      build_active_frame(
          cycle_sequence,
          batch.frames[0U]);
      build_active_frame(
          cycle_sequence + 1U,
          batch.frames[1U]);
      return actuation::ActuatorProgramStatus::ok;
    }

    if (zero_active) {
      batch.count = 0U;
      return actuation::ActuatorProgramStatus::ok;
    }

    batch.count = 1U;
    build_active_frame(
        cycle_sequence,
        batch.frames[0U]);
    return actuation::ActuatorProgramStatus::ok;
  }

  [[nodiscard]] actuation::ActuatorProgramStatus
  render_safe_stop(
      const actuation::SafeStopReason reason,
      actuation::CyclicCanFrameBatch& batch) noexcept override {
    ++safe_render_calls;
    last_safe_reason = reason;
    batch = {};

    if (fault_safe) {
      return actuation::ActuatorProgramStatus::fault;
    }

    if (silent_safe) {
      return actuation::ActuatorProgramStatus::ok;
    }

    batch.count = 1U;
    auto& frame = batch.frames[0U];
    frame.identifier = 0x180U;
    frame.identifier_format =
        transport::CanIdentifierFormat::standard_11_bit;
    frame.format = transport::CanFrameFormat::classic;
    frame.type = transport::CanFrameType::data;
    frame.length = 2U;
    frame.payload[0U] = std::byte{0xFFU};
    frame.payload[1U] = static_cast<std::byte>(
        static_cast<std::uint8_t>(reason));
    return actuation::ActuatorProgramStatus::ok;
  }

  static void build_active_frame(
      const std::uint32_t sequence,
      transport::CanFrame& frame) noexcept {
    frame = {};
    frame.identifier = 0x180U;
    frame.identifier_format =
        transport::CanIdentifierFormat::standard_11_bit;
    frame.format = transport::CanFrameFormat::classic;
    frame.type = transport::CanFrameType::data;
    frame.length = 2U;
    frame.payload[0U] = static_cast<std::byte>(
        static_cast<std::uint8_t>(sequence & 0xFFU));
    frame.payload[1U] = std::byte{0x28U};
  }

  actuation::CyclicCanActuatorProgramContract contract{
      1U,
      1U,
      time::MonotonicDuration{1000},
      time::MonotonicDuration{1000}};
  bool fault_active{false};
  bool fault_safe{false};
  bool silent_safe{false};
  bool zero_active{false};
  bool oversized_active{false};
  std::uint32_t active_render_calls{0U};
  std::uint32_t safe_render_calls{0U};
  actuation::SafeStopReason last_safe_reason{
      actuation::SafeStopReason::explicit_stop};
};

transport::CanChannelConfig bus_config() {
  return {
      500000U,
      true,
      2000000U,
      transport::CanMode::normal,
      kClockDomain};
}

actuation::CyclicCanActuatorConfig actuator_config(
    const bool feedback_required = true) {
  return {
      time::MonotonicDuration{10000000},
      time::MonotonicDuration{2000000},
      time::MonotonicDuration{100000000},
      feedback_required
          ? time::MonotonicDuration{30000000}
          : time::MonotonicDuration{0}};
}

bool start_bus(
    FakeCanDriver& driver,
    transport::CanBusRuntime& bus) {
  (void)driver;
  return bus.freeze_configuration() &&
         bus.start(bus_config()) ==
             transport::CanStatus::ok;
}

bool start_runtime(
    actuation::CyclicCanActuatorRuntime& runtime,
    const actuation::CyclicCanActuatorConfig& config) {
  return runtime.configure(config) &&
         runtime.start() ==
             actuation::CyclicCanActuatorStatus::ok;
}

}  // namespace

int main() {
  int failures = 0;

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "basic bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config()) &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::interlocked,
        "actuator runtime starts fail-safe interlocked");

    failures += require(
        runtime.activate() ==
                actuation::CyclicCanActuatorStatus::invalid_state &&
            driver.sent_count == 0U,
        "activation cannot bypass interlock");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "cadence bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config()) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "fresh command and open interlock activate cyclic control");

    failures += require(
        runtime.state() ==
                actuation::CyclicCanActuatorState::active &&
            driver.sent_count == 1U &&
            std::to_integer<std::uint8_t>(
                driver.sent[0U].payload[0U]) == 0U &&
            runtime.next_service_deadline() ==
                time::MonotonicTime{10000000},
        "activation sends first cycle immediately");

    clock.set_ns(9000000);
    failures += require(
        runtime.service() ==
                actuation::CyclicCanActuatorStatus::no_action &&
            driver.sent_count == 1U,
        "service does not transmit before deadline");

    clock.set_ns(10000000);
    failures += require(
        runtime.note_valid_feedback() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.service() ==
                actuation::CyclicCanActuatorStatus::ok &&
            driver.sent_count == 2U &&
            std::to_integer<std::uint8_t>(
                driver.sent[1U].payload[0U]) == 1U &&
            runtime.next_service_deadline() ==
                time::MonotonicTime{20000000},
        "due service sends exactly one next cyclic frame");

    const auto budget = runtime.execution_budget();
    failures += require(
        budget.valid &&
            budget.max_service_duration.count() > 0 &&
            budget.max_activate_duration.count() > 0 &&
            budget.max_safe_stop_duration.count() > 0,
        "runtime exposes bounded execution budget");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "command-timeout bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "command-timeout setup");

    clock.set_ns(100000000);
    failures += require(
        runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::command_timeout &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted &&
            driver.sent_count == 2U &&
            std::to_integer<std::uint8_t>(
                driver.sent[1U].payload[0U]) == 0xFFU &&
            program.last_safe_reason ==
                actuation::SafeStopReason::command_timeout &&
            runtime.counters().command_timeouts == 1U,
        "late command refresh cannot revive expired active lease");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "feedback-timeout bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config()) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "feedback-timeout setup");

    clock.set_ns(30000000);
    failures += require(
        runtime.note_valid_feedback() ==
                actuation::CyclicCanActuatorStatus::feedback_timeout &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted &&
            program.last_safe_reason ==
                actuation::SafeStopReason::feedback_timeout &&
            runtime.counters().feedback_timeouts == 1U,
        "late feedback cannot revive expired feedback lease");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "feedback-refresh bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config()) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "feedback-refresh setup");

    clock.set_ns(20000000);
    failures += require(
        runtime.note_valid_feedback() ==
                actuation::CyclicCanActuatorStatus::ok,
        "valid feedback refreshes lease before expiry");

    clock.set_ns(40000000);
    failures += require(
        runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.service() ==
                actuation::CyclicCanActuatorStatus::cadence_missed,
        "feedback lease survives while cadence miss remains independently fatal");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "cadence-miss bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "cadence-miss setup");

    clock.set_ns(12000001);
    failures += require(
        runtime.service() ==
                actuation::CyclicCanActuatorStatus::cadence_missed &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted &&
            program.last_safe_reason ==
                actuation::SafeStopReason::cadence_missed &&
            runtime.counters().cadence_misses == 1U,
        "service lateness beyond bound fails closed");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "interlock bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "interlock trip setup");

    failures += require(
        runtime.set_interlock(false) ==
                actuation::CyclicCanActuatorStatus::interlocked &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::interlocked &&
            driver.sent_count == 2U &&
            program.last_safe_reason ==
                actuation::SafeStopReason::interlock_opened &&
            runtime.counters().interlock_trips == 1U,
        "opening interlock sends safe-stop immediately");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "explicit-stop bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.stop() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::stopped &&
            program.last_safe_reason ==
                actuation::SafeStopReason::explicit_stop,
        "explicit stop neutralizes active DUT");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "program-fault bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "program-fault setup");

    program.fault_active = true;
    clock.set_ns(10000000);
    failures += require(
        runtime.service() ==
                actuation::CyclicCanActuatorStatus::program_fault &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted &&
            program.last_safe_reason ==
                actuation::SafeStopReason::program_fault &&
            runtime.counters().program_faults == 1U,
        "profile render fault triggers bounded safe-stop");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "transport-fault bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "transport-fault setup");

    driver.next_send_status = transport::CanStatus::would_block;
    clock.set_ns(10000000);
    failures += require(
        runtime.service() ==
                actuation::CyclicCanActuatorStatus::transport_fault &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted &&
            driver.sent_count == 2U &&
            program.last_safe_reason ==
                actuation::SafeStopReason::transport_fault &&
            runtime.counters().transport_faults == 1U,
        "TX backpressure faults control and sends safe-stop when possible");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "safe-stop-failure bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "safe-stop-failure setup");

    program.fault_safe = true;
    failures += require(
        runtime.set_interlock(false) ==
                actuation::CyclicCanActuatorStatus::safe_stop_failed &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted &&
            runtime.counters().safe_stop_failures == 1U,
        "failed safe-stop is explicit and latched");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    program.silent_safe = true;
    program.contract.max_safe_stop_frames = 0U;
    failures += require(
        start_bus(driver, bus),
        "silent-safe bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.stop() ==
                actuation::CyclicCanActuatorStatus::ok &&
            driver.sent_count == 1U,
        "profile may define safe-stop as silence");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "clock-fault bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "clock-fault setup");

    clock.status = time::MonotonicClockStatus::discontinuity;
    failures += require(
        runtime.service() ==
                actuation::CyclicCanActuatorStatus::clock_fault &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted &&
            program.last_safe_reason ==
                actuation::SafeStopReason::clock_fault,
        "clock discontinuity fails closed");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    program.zero_active = true;
    failures += require(
        start_bus(driver, bus),
        "zero-active bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::program_fault &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::faulted,
        "active cycle cannot silently emit zero command frames");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    program.oversized_active = true;
    failures += require(
        start_bus(driver, bus),
        "oversized-active bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::program_fault,
        "profile cannot exceed declared active frame bound");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    program.contract.max_active_render_duration =
        time::MonotonicDuration{10000000};
    failures += require(
        start_bus(driver, bus),
        "budget-reject bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        !runtime.configure(actuator_config(false)),
        "unsustainable active-cycle execution budget is rejected");
  }

  {
    FakeCanDriver driver;
    transport::CanBusRuntime bus{driver};
    FakeClock clock;
    TestProgram program;
    failures += require(
        start_bus(driver, bus),
        "fault-reset bus starts");

    actuation::CyclicCanActuatorRuntime runtime{
        bus,
        clock,
        program};
    failures += require(
        start_runtime(runtime, actuator_config(false)) &&
            runtime.refresh_command() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.set_interlock(true) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.activate() ==
                actuation::CyclicCanActuatorStatus::ok,
        "fault-reset setup");

    clock.set_ns(12000001);
    failures += require(
        runtime.service() ==
                actuation::CyclicCanActuatorStatus::cadence_missed &&
            runtime.reset_fault() ==
                actuation::CyclicCanActuatorStatus::interlocked,
        "fault cannot reset while external interlock remains open");

    failures += require(
        runtime.set_interlock(false) ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.reset_fault() ==
                actuation::CyclicCanActuatorStatus::ok &&
            runtime.state() ==
                actuation::CyclicCanActuatorState::stopped,
        "fault recovery requires explicit closed interlock and restart");
  }

  if (failures == 0) {
    std::cout
        << "CORE_V2_CYCLIC_CAN_ACTUATOR_RUNTIME=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
