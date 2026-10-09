#include "ecu/bench/event_bus_publisher.hpp"
#include "ecu/bench/session.hpp"

#include <cstddef>
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
        {7U},
        time::MonotonicDuration{1},
        time::MonotonicDuration{5},
        time::MonotonicDuration{0},
        true};
  }

  [[nodiscard]] time::MonotonicClockReading read()
      const noexcept override {
    return {
        time::MonotonicClockStatus::ok,
        {7U},
        now,
        time::MonotonicDuration{0}};
  }

  mutable time::MonotonicTime now{1000};
};

class CapturingSink final : public runtime::IEventSink {
 public:
  void on_event(const runtime::EventView& event) noexcept override {
    ++calls;
    last_type = event.header.type;
    last_correlation = event.header.correlation_id;
    decoded_ok = bench::decode_bench_session_event_v1(
        event.payload,
        event.payload_size,
        decoded);
  }

  std::uint32_t calls{0U};
  runtime::EventTypeId last_type{0U};
  runtime::CorrelationId last_correlation{0U};
  bool decoded_ok{false};
  bench::DecodedBenchSessionEventV1 decoded{};
};

class SimpleEndpoint final : public bench::IDutSessionEndpoint {
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
    return bench::BenchComponentStatus::no_action;
  }
  [[nodiscard]] bench::BenchComponentStatus safe_stop() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
  [[nodiscard]] bench::BenchComponentStatus stop() noexcept override {
    return bench::BenchComponentStatus::ok;
  }
};

class ReentrantSink final : public runtime::IEventSink {
 public:
  void on_event(const runtime::EventView&) noexcept override {
    ++calls;
    if (session != nullptr) {
      nested = session->stop();
    }
  }

  bench::BenchSession* session{nullptr};
  bench::BenchSessionStatus nested{
      bench::BenchSessionStatus::invalid_state};
  std::uint32_t calls{0U};
};

class FailingPublisher final
    : public bench::IBenchSessionEventPublisher {
 public:
  [[nodiscard]] bool ready() const noexcept override {
    return ready_value;
  }

  [[nodiscard]] bench::BenchSessionEventPublisherExecutionContract
  execution_contract() const noexcept override {
    return {time::MonotonicDuration{15}};
  }

  [[nodiscard]] bench::BenchSessionEventPublishStatus publish(
      const bench::BenchSessionEvent&) noexcept override {
    ++calls;
    return result;
  }

  bool ready_value{true};
  bench::BenchSessionEventPublishStatus result{
      bench::BenchSessionEventPublishStatus::unavailable};
  std::uint32_t calls{0U};
};

domain::DutDescriptor make_dut(
    const domain::DutProfileId profile_id) {
  return {
      profile_id,
      domain::DutClass::actuator,
      domain::kTruck,
      domain::dut_capability_mask(
          domain::DutCapability::raw_can)};
}

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

  {
    FakeClock clock;
    runtime::EventBus events{clock};
    CapturingSink sink;
    failures += require(
        events.subscribe(
            bench::kBenchSessionEventType,
            sink,
            {time::MonotonicDuration{20}}) ==
                runtime::EventSubscriptionStatus::subscribed &&
            events.freeze_configuration(),
        "Core EventBus fixture freezes");

    bench::BenchSessionEventBusPublisher publisher{events};
    const auto contract = publisher.execution_contract();
    failures += require(
        publisher.ready() &&
            contract.max_publish_duration ==
                time::MonotonicDuration{25},
        "Bench publisher exposes bounded Core EventBus budget");

    bench::BenchSessionEvent event{};
    event.kind = bench::BenchSessionEventKind::fault;
    event.previous_state = bench::BenchSessionState::running;
    event.snapshot.lifecycle_revision = 9U;
    event.snapshot.state = bench::BenchSessionState::faulted;
    event.snapshot.status = bench::BenchSessionStatus::dut_fault;
    event.snapshot.reason = bench::BenchSessionReason::dut_fault;
    event.snapshot.fault_source = bench::BenchFaultSource::dut;
    event.snapshot.dut_profile_id = 0x44556677U;
    event.snapshot.operation_generation = 42U;
    event.snapshot.last_completed_operation_generation = 41U;
    event.snapshot.active_resource_count = 2U;
    event.snapshot.configured = true;
    event.snapshot.cleanup_required = true;
    event.snapshot.counters.faults = 3U;

    failures += require(
        publisher.publish(event) ==
                bench::BenchSessionEventPublishStatus::published &&
            sink.calls == 1U &&
            sink.last_type == bench::kBenchSessionEventType &&
            sink.last_correlation == 42U &&
            sink.decoded_ok &&
            sink.decoded.kind == bench::BenchSessionEventKind::fault &&
            sink.decoded.previous_state ==
                bench::BenchSessionState::running &&
            sink.decoded.state == bench::BenchSessionState::faulted &&
            sink.decoded.reason == bench::BenchSessionReason::dut_fault &&
            sink.decoded.dut_profile_id == 0x44556677U &&
            sink.decoded.lifecycle_revision == 9U &&
            sink.decoded.operation_generation == 42U &&
            sink.decoded.cleanup_required &&
            sink.decoded.faults == 3U,
        "versioned Bench event encoding round-trips through Core EventBus");

    bench::BenchSessionEventWireBufferV1 malformed{};
    failures += require(
        bench::encode_bench_session_event_v1(event, malformed),
        "wire encoder accepts valid v1 event");
    malformed[10U] = std::byte{0x01U};
    bench::DecodedBenchSessionEventV1 decoded{};
    failures += require(
        !bench::decode_bench_session_event_v1(
            malformed.data(),
            malformed.size(),
            decoded),
        "wire decoder fails closed on non-zero reserved byte");
  }

  {
    FakeClock clock;
    runtime::EventBus events{clock};
    CapturingSink capture;
    ReentrantSink reentrant;

    failures += require(
        events.subscribe(
            bench::kBenchSessionEventType,
            capture,
            {time::MonotonicDuration{20}}) ==
                runtime::EventSubscriptionStatus::subscribed &&
            events.subscribe(
                bench::kBenchSessionEventType,
                reentrant,
                {time::MonotonicDuration{30}}) ==
                runtime::EventSubscriptionStatus::subscribed &&
            events.freeze_configuration(),
        "session observability EventBus freezes");

    bench::BenchSessionEventBusPublisher publisher{events};
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registered = duts.register_dut(make_dut(0x3001U));
    failures += require(
        registered.status == runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "observability DUT registry freezes");

    SimpleEndpoint endpoint;
    bench::BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr,
        &publisher};
    reentrant.session = &session;

    failures += require(
        session.configure(make_config(registered.handle, 501U)) &&
            session.status() == bench::BenchSessionStatus::ok &&
            reentrant.nested == bench::BenchSessionStatus::busy,
        "event callback cannot reenter and mutate Bench Session");

    auto snapshot = session.snapshot();
    failures += require(
        snapshot.schema_version ==
                bench::BenchSessionSnapshot::kSchemaVersion &&
            snapshot.lifecycle_revision == 1U &&
            snapshot.state == bench::BenchSessionState::ready &&
            snapshot.reason == bench::BenchSessionReason::configured &&
            snapshot.dut_profile_id == 0x3001U &&
            snapshot.configured &&
            !snapshot.observability_degraded,
        "configured snapshot is stable and typed");

    const auto budget = session.execution_budget();
    failures += require(
        budget.valid &&
            budget.max_start_duration ==
                time::MonotonicDuration{175} &&
            budget.max_service_duration ==
                time::MonotonicDuration{175} &&
            budget.max_stop_duration ==
                time::MonotonicDuration{145} &&
            budget.max_recover_duration ==
                time::MonotonicDuration{145},
        "event publication is included in Bench execution budgets");

    failures += require(
        session.start() == bench::BenchSessionStatus::ok,
        "observed session starts");
    snapshot = session.snapshot();
    failures += require(
        snapshot.state == bench::BenchSessionState::running &&
            snapshot.reason == bench::BenchSessionReason::started &&
            snapshot.lifecycle_revision == 2U &&
            snapshot.operation_generation != 0U &&
            snapshot.active_resource_count == 1U &&
            capture.decoded.state == bench::BenchSessionState::running &&
            capture.decoded.previous_state ==
                bench::BenchSessionState::ready,
        "start emits one final lifecycle event with active generation");

    const auto running_revision = snapshot.lifecycle_revision;
    failures += require(
        session.service() == bench::BenchSessionStatus::no_action &&
            session.snapshot().lifecycle_revision ==
                running_revision + 1U,
        "service updates snapshot revision without publishing lifecycle event");

    const auto active = session.cancellation_token();
    failures += require(
        session.request_cancel(active) ==
                bench::BenchSessionCancelRequestStatus::requested &&
            session.snapshot().cancellation_requested,
        "cancellation request is visible in snapshot");

    failures += require(
        session.service() == bench::BenchSessionStatus::cancelled,
        "requested cancellation safely stops observed session");
    snapshot = session.snapshot();
    failures += require(
        snapshot.state == bench::BenchSessionState::ready &&
            snapshot.reason == bench::BenchSessionReason::cancelled &&
            snapshot.operation_generation == 0U &&
            snapshot.last_completed_operation_generation ==
                active.generation &&
            !snapshot.cancellation_requested,
        "cancel event retains completed operation generation");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registered = duts.register_dut(make_dut(0x3002U));
    failures += require(
        registered.status == runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "degraded-observability DUT registry freezes");

    SimpleEndpoint endpoint;
    FailingPublisher publisher;
    bench::BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr,
        &publisher};

    failures += require(
        session.configure(make_config(registered.handle, 502U)),
        "telemetry publication failure does not reject valid session config");

    const auto snapshot = session.snapshot();
    failures += require(
        snapshot.state == bench::BenchSessionState::ready &&
            snapshot.observability_degraded &&
            snapshot.counters.event_publish_failures == 1U &&
            publisher.calls == 1U,
        "event delivery failure is fail-visible without corrupting lifecycle");
  }

  {
    runtime::ResourceManager resources;
    runtime::DutRegistry duts;
    const auto registered = duts.register_dut(make_dut(0x3003U));
    failures += require(
        registered.status == runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "unready publisher DUT registry freezes");

    SimpleEndpoint endpoint;
    FailingPublisher publisher;
    publisher.ready_value = false;
    bench::BenchSession session{
        resources,
        duts,
        endpoint,
        nullptr,
        nullptr,
        &publisher};

    failures += require(
        !session.configure(make_config(registered.handle, 503U)) &&
            session.status() ==
                bench::BenchSessionStatus::execution_contract_invalid,
        "unready event publisher fails configuration contract closed");
  }

  if (failures == 0) {
    std::cout << "BENCH_RUNTIME_OBSERVABILITY_V1=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
