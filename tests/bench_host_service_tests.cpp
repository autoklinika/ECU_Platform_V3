#include "ecu/bench/host_service.hpp"
#include "ecu/bench/session.hpp"

#include <cstdint>
#include <iostream>

namespace {

namespace bench = ecu::bench;
namespace domain = ecu::core::v2::domain;
namespace runtime = ecu::core::v2::runtime;
namespace time = ecu::core::v2::time;

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
        {11U},
        time::MonotonicDuration{1},
        time::MonotonicDuration{5},
        time::MonotonicDuration{0},
        true};
  }

  [[nodiscard]] time::MonotonicClockReading read()
      const noexcept override {
    return {
        status,
        {11U},
        now,
        time::MonotonicDuration{0}};
  }

  mutable time::MonotonicClockStatus status{
      time::MonotonicClockStatus::ok};
  mutable time::MonotonicTime now{1000};
};

class Endpoint final : public bench::IDutSessionEndpoint {
 public:
  [[nodiscard]] bench::BenchComponentExecutionContract
  execution_contract() const noexcept override {
    return {
        time::MonotonicDuration{10},
        time::MonotonicDuration{20},
        time::MonotonicDuration{30},
        time::MonotonicDuration{40},
        time::MonotonicDuration{50}};
  }

  [[nodiscard]] bench::BenchComponentStatus prepare() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus activate() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus service() noexcept override {
    return service_status;
  }
  [[nodiscard]] bench::BenchComponentStatus safe_stop() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus stop() noexcept override {
    return bench::BenchComponentStatus::ok;
  }

  bench::BenchComponentStatus service_status{
      bench::BenchComponentStatus::no_action};
};

bench::BenchSessionConfig make_config(
    const runtime::DutHandle handle,
    const runtime::ResourceOwnerId owner) {
  bench::BenchSessionConfig config{};
  config.session_owner = owner;
  config.dut = handle;
  return config;
}

}  // namespace

int main() {
  int failures = 0;

  runtime::ResourceManager resources;
  runtime::DutRegistry duts;
  const auto registered = duts.register_dut({
      0x5001U,
      domain::DutClass::actuator,
      domain::kTruck,
      domain::dut_capability_mask(
          domain::DutCapability::raw_can)});
  failures += require(
      registered.status == runtime::DutRegistrationStatus::registered &&
          duts.freeze_configuration(),
      "host-service DUT topology freezes");

  Endpoint endpoint;
  bench::BenchSession session{
      resources,
      duts,
      endpoint,
      nullptr,
      nullptr};
  failures += require(
      session.configure(make_config(registered.handle, 601U)),
      "host-service session configures");

  FakeClock clock;
  bench::BenchSessionHostRuntime host{session, clock};

  failures += require(
      !host.configure({time::MonotonicDuration{130}}),
      "host rejects timeout that cannot contain worst-case service path");

  failures += require(
      host.configure({time::MonotonicDuration{200}}) &&
          host.configured() &&
          !host.armed(),
      "host accepts explicit bounded service timeout");

  const auto budget = host.execution_budget();
  failures += require(
      budget.valid &&
          budget.max_start_duration ==
              time::MonotonicDuration{215} &&
          budget.max_service_duration ==
              time::MonotonicDuration{220} &&
          budget.max_stop_duration ==
              time::MonotonicDuration{90} &&
          budget.max_recover_duration ==
              time::MonotonicDuration{90},
      "host runtime exposes conservative execution budget");

  auto result = host.start();
  failures += require(
      result.status == bench::BenchHostServiceStatus::ok &&
          result.session_status == bench::BenchSessionStatus::ok &&
          host.armed() &&
          session.state() == bench::BenchSessionState::running,
      "host start arms deadline only after session reaches running");

  clock.now = time::MonotonicTime{1100};
  result = host.service();
  failures += require(
      result.status == bench::BenchHostServiceStatus::no_action &&
          result.session_status ==
              bench::BenchSessionStatus::no_action &&
          host.armed() &&
          session.state() == bench::BenchSessionState::running,
      "timely host service polls session and kicks watchdog");

  clock.now = time::MonotonicTime{1400};
  result = host.service();
  failures += require(
      result.status ==
              bench::BenchHostServiceStatus::deadline_missed &&
          !host.armed() &&
          session.state() == bench::BenchSessionState::ready &&
          resources.active_count() == 0U,
      "missed host deadline triggers fail-closed session stop");

  clock.now = time::MonotonicTime{1500};
  result = host.start();
  failures += require(
      result.status == bench::BenchHostServiceStatus::ok &&
          host.armed(),
      "host restarts after safe deadline stop");

  clock.status = time::MonotonicClockStatus::unavailable;
  result = host.service();
  failures += require(
      result.status == bench::BenchHostServiceStatus::clock_fault &&
          !host.armed() &&
          session.state() == bench::BenchSessionState::ready,
      "host clock fault triggers fail-closed session stop");

  clock.status = time::MonotonicClockStatus::ok;
  clock.now = time::MonotonicTime{2000};
  result = host.start();
  failures += require(
      result.status == bench::BenchHostServiceStatus::ok &&
          host.armed(),
      "host starts cancellation fixture");

  const auto token = session.cancellation_token();
  failures += require(
      host.request_cancel(token) ==
          bench::BenchSessionCancelRequestStatus::requested,
      "host forwards generation-safe cancellation request");

  clock.now = time::MonotonicTime{2050};
  result = host.service();
  failures += require(
      result.status == bench::BenchHostServiceStatus::ok &&
          result.session_status == bench::BenchSessionStatus::cancelled &&
          !host.armed() &&
          session.state() == bench::BenchSessionState::ready,
      "cancelled session automatically disarms host watchdog");

  clock.now = time::MonotonicTime{3000};
  clock.status = time::MonotonicClockStatus::unavailable;
  result = host.start();
  failures += require(
      result.status == bench::BenchHostServiceStatus::clock_fault &&
          !host.armed() &&
          session.state() == bench::BenchSessionState::ready &&
          resources.active_count() == 0U,
      "watchdog arm failure after DUT start performs immediate safe stop");

  clock.status = time::MonotonicClockStatus::ok;
  failures += require(
      host.stop().status == bench::BenchHostServiceStatus::no_action,
      "host stop is idempotent when session is already ready");

  if (failures == 0) {
    std::cout << "BENCH_RUNTIME_HOST_SERVICE_V1=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
