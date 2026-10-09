
#include "ecu/core_v2/domain/device_under_test.hpp"
#include "ecu/core_v2/runtime/cancellation.hpp"
#include "ecu/core_v2/runtime/command_dispatcher.hpp"
#include "ecu/core_v2/runtime/dut_registry.hpp"
#include "ecu/core_v2/runtime/event_bus.hpp"
#include "ecu/core_v2/runtime/lifecycle.hpp"
#include "ecu/core_v2/runtime/module_registry.hpp"
#include "ecu/core_v2/runtime/resource_manager.hpp"
#include "ecu/core_v2/runtime/state_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using namespace ecu::core::v2;

constexpr time::MonotonicClockDomainId kDomain{0x52554E54U};

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
        kDomain,
        time::MonotonicDuration{1},
        time::MonotonicDuration{100},
        time::MonotonicDuration{10},
        true};
  }

  [[nodiscard]] time::MonotonicClockReading read()
      const noexcept override {
    return {
        status,
        kDomain,
        now,
        uncertainty};
  }

  time::MonotonicClockStatus status{
      time::MonotonicClockStatus::ok};
  time::MonotonicTime now{1000};
  time::MonotonicDuration uncertainty{0};
};

class TestHandler final : public runtime::ICommandHandler {
 public:
  [[nodiscard]] runtime::CommandResult handle(
      const runtime::CommandView& command) noexcept override {
    ++calls;
    last_id = command.header.id;

    if (dispatcher != nullptr && nested_command != nullptr) {
      nested_result = dispatcher->dispatch(*nested_command);
    }

    return {runtime::CommandOutcome::succeeded, 0U};
  }

  runtime::CommandDispatcher* dispatcher{nullptr};
  const runtime::CommandView* nested_command{nullptr};
  runtime::CommandDispatchResult nested_result{};
  std::uint32_t calls{0U};
  runtime::CommandId last_id{0U};
};

class DenyPolicy final : public runtime::ICommandPolicy {
 public:
  [[nodiscard]] runtime::CommandPolicyDecision evaluate(
      const runtime::CommandView&) const noexcept override {
    return runtime::CommandPolicyDecision::deny;
  }
};

class TestStateProvider final : public runtime::IStateProvider {
 public:
  [[nodiscard]] runtime::StateTypeId state_type()
      const noexcept override {
    return 0x1001U;
  }

  [[nodiscard]] runtime::StateRevision revision()
      const noexcept override {
    return revision_value;
  }

  [[nodiscard]] runtime::StateSnapshotStatus snapshot(
      std::byte* destination,
      const std::size_t capacity,
      std::size_t& length,
      runtime::StateHeader& header) const noexcept override {
    if (capacity < payload.size()) {
      length = payload.size();
      return runtime::StateSnapshotStatus::buffer_too_small;
    }
    if (destination == nullptr) {
      return runtime::StateSnapshotStatus::invalid_argument;
    }

    for (std::size_t index = 0U;
         index < payload.size();
         ++index) {
      destination[index] = payload[index];
    }
    length = payload.size();
    header.revision = revision_value;
    header.updated_at = time::MonotonicTime{12345};
    return runtime::StateSnapshotStatus::ok;
  }

  std::array<std::byte, 3U> payload{
      std::byte{0x11U},
      std::byte{0x22U},
      std::byte{0x33U}};
  runtime::StateRevision revision_value{7U};
};

class CountingEventSink final : public runtime::IEventSink {
 public:
  void on_event(const runtime::EventView& event) noexcept override {
    ++calls;
    last_sequence = event.header.sequence;
    last_type = event.header.type;
    last_timestamp = event.header.timestamp;
  }

  std::uint32_t calls{0U};
  runtime::EventSequence last_sequence{0U};
  runtime::EventTypeId last_type{0U};
  time::MonotonicTime last_timestamp{0};
};

class ReentrantEventSink final : public runtime::IEventSink {
 public:
  void on_event(const runtime::EventView&) noexcept override {
    ++calls;
    if (bus != nullptr) {
      nested = bus->publish({
          0x2002U,
          0U,
          runtime::EventSeverity::warning,
          nullptr,
          0U});
    }
  }

  runtime::EventBus* bus{nullptr};
  runtime::EventPublishResult nested{};
  std::uint32_t calls{0U};
};

class TestModule final : public runtime::ICoreModule {
 public:
  explicit TestModule(const runtime::ModuleId id) noexcept
      : descriptor_value{
            id,
            runtime::ModuleKind::bench_service,
            0x1U} {}

  [[nodiscard]] runtime::ModuleDescriptor descriptor()
      const noexcept override {
    return descriptor_value;
  }

  [[nodiscard]] runtime::LifecycleState state()
      const noexcept override {
    return lifecycle.state();
  }

  [[nodiscard]] runtime::LifecycleStatus start()
      noexcept override {
    const auto first = lifecycle.begin_start();
    if (first != runtime::LifecycleStatus::ok &&
        first != runtime::LifecycleStatus::no_action) {
      return first;
    }
    return lifecycle.mark_running();
  }

  [[nodiscard]] runtime::LifecycleStatus service()
      noexcept override {
    return lifecycle.state() == runtime::LifecycleState::running
               ? runtime::LifecycleStatus::ok
               : runtime::LifecycleStatus::invalid_transition;
  }

  [[nodiscard]] runtime::LifecycleStatus stop()
      noexcept override {
    const auto first = lifecycle.begin_stop();
    if (first != runtime::LifecycleStatus::ok &&
        first != runtime::LifecycleStatus::no_action) {
      return first;
    }
    return lifecycle.mark_stopped();
  }

  runtime::ModuleDescriptor descriptor_value{};
  runtime::LifecycleStateMachine lifecycle{};
};

runtime::CommandView command(
    const runtime::CommandId id,
    const runtime::CommandTypeId type) {
  runtime::CommandView value{};
  value.header.id = id;
  value.header.type = type;
  value.header.correlation_id = id + 100U;
  value.header.priority = runtime::CommandPriority::normal;
  value.header.issued_at = time::MonotonicTime{1000};
  return value;
}

domain::DutDescriptor actuator_dut(
    const domain::DutProfileId profile_id) {
  return {
      profile_id,
      domain::DutClass::actuator,
      domain::kTruck,
      static_cast<domain::DutCapabilityMask>(
          domain::dut_capability_mask(
              domain::DutCapability::raw_can) |
          domain::dut_capability_mask(
              domain::DutCapability::cyclic_can) |
          domain::dut_capability_mask(
              domain::DutCapability::active_control) |
          domain::dut_capability_mask(
              domain::DutCapability::feedback))};
}

}  // namespace

int main() {
  int failures = 0;

  {
    runtime::LifecycleStateMachine lifecycle;
    failures += require(
        lifecycle.state() == runtime::LifecycleState::stopped &&
            lifecycle.begin_start() ==
                runtime::LifecycleStatus::ok &&
            lifecycle.mark_running() ==
                runtime::LifecycleStatus::ok &&
            lifecycle.begin_stop() ==
                runtime::LifecycleStatus::ok &&
            lifecycle.mark_stopped() ==
                runtime::LifecycleStatus::ok &&
            lifecycle.state() ==
                runtime::LifecycleState::stopped,
        "lifecycle follows explicit start/run/stop transitions");

    failures += require(
        lifecycle.mark_running() ==
                runtime::LifecycleStatus::invalid_transition,
        "lifecycle rejects invalid transition");

    lifecycle.mark_faulted();
    failures += require(
        lifecycle.state() == runtime::LifecycleState::faulted &&
            lifecycle.reset_fault() ==
                runtime::LifecycleStatus::ok &&
            lifecycle.state() ==
                runtime::LifecycleState::stopped,
        "lifecycle fault reset returns to stopped only");
  }

  {
    runtime::CancellationSource cancellation;
    const auto first = cancellation.begin();
    failures += require(
        first.status ==
                runtime::CancellationBeginStatus::started &&
            first.token.valid() &&
            cancellation.active() &&
            !cancellation.requested(first.token),
        "cancellation source begins generation-scoped operation");

    failures += require(
        cancellation.request(first.token) ==
                runtime::CancellationRequestStatus::requested &&
            cancellation.requested(first.token) &&
            cancellation.request(first.token) ==
                runtime::CancellationRequestStatus::
                    already_requested &&
            cancellation.complete(first.token) ==
                runtime::CancellationCompleteStatus::completed &&
            !cancellation.active(),
        "cancellation request is idempotent and explicitly completed");

    const auto second = cancellation.begin();
    failures += require(
        second.status ==
                runtime::CancellationBeginStatus::started &&
            second.token.valid() &&
            second.token.generation != first.token.generation &&
            cancellation.request(first.token) ==
                runtime::CancellationRequestStatus::stale_token &&
            !cancellation.requested(first.token) &&
            !cancellation.requested(second.token),
        "stale cancellation token cannot affect newer operation");

    failures += require(
        cancellation.begin().status ==
                runtime::CancellationBeginStatus::busy &&
            cancellation.complete(second.token) ==
                runtime::CancellationCompleteStatus::completed,
        "cancellation source allows one explicit active generation");
  }

  {
    runtime::ResourceManager resources;
    const runtime::ResourceKey can0{
        runtime::ResourceClass::can_channel,
        1U};

    const auto first = resources.acquire(can0, 100U);
    failures += require(
        first.status == runtime::ResourceAcquireStatus::acquired &&
            first.lease.valid() &&
            resources.owns(first.lease) &&
            resources.active_count() == 1U,
        "resource manager grants generation-safe lease");

    const auto same = resources.acquire(can0, 100U);
    const auto other = resources.acquire(can0, 200U);
    failures += require(
        same.status ==
                runtime::ResourceAcquireStatus::already_owned &&
            same.lease.generation == first.lease.generation &&
            other.status == runtime::ResourceAcquireStatus::busy &&
            other.lease.owner == 100U,
        "resource manager reports same owner and contention explicitly");

    failures += require(
        resources.release(first.lease) ==
                runtime::ResourceReleaseStatus::released &&
            resources.active_count() == 0U,
        "resource lease releases cleanly");

    const auto second = resources.acquire(can0, 200U);
    failures += require(
        second.status == runtime::ResourceAcquireStatus::acquired &&
            second.lease.generation != first.lease.generation &&
            resources.release(first.lease) ==
                runtime::ResourceReleaseStatus::not_owner &&
            resources.owns(second.lease),
        "stale generation cannot release newer owner");
  }

  {
    runtime::AllowAllCommandPolicy policy;
    runtime::CommandDispatcher dispatcher{
        policy,
        {time::MonotonicDuration{50}}};
    TestHandler handler;
    const auto first_command = command(1U, 0x1001U);

    failures += require(
        dispatcher.register_handler(
            0x1001U,
            handler,
            {time::MonotonicDuration{200}}) ==
                runtime::CommandRegistrationStatus::registered &&
            dispatcher.dispatch(first_command).status ==
                runtime::CommandDispatchStatus::
                    configuration_not_frozen &&
            dispatcher.freeze_configuration(),
        "command topology is fixed before dispatch");

    const auto result = dispatcher.dispatch(first_command);
    const auto budget =
        dispatcher.max_dispatch_duration(0x1001U);
    failures += require(
        result.status ==
                runtime::CommandDispatchStatus::dispatched &&
            result.result.outcome ==
                runtime::CommandOutcome::succeeded &&
            handler.calls == 1U &&
            handler.last_id == 1U &&
            budget.valid &&
            budget.max_duration ==
                time::MonotonicDuration{250},
        "command dispatch is bounded and typed");

    failures += require(
        dispatcher.register_handler(
            0x1002U,
            handler,
            {time::MonotonicDuration{200}}) ==
                runtime::CommandRegistrationStatus::
                    configuration_frozen,
        "command registration is rejected after freeze");
  }

  {
    runtime::AllowAllCommandPolicy policy;
    runtime::CommandDispatcher dispatcher{
        policy,
        {time::MonotonicDuration{10}}};
    TestHandler handler;
    const auto outer = command(10U, 0x2001U);
    const auto nested = command(11U, 0x2001U);
    handler.dispatcher = &dispatcher;
    handler.nested_command = &nested;

    failures += require(
        dispatcher.register_handler(
            0x2001U,
            handler,
            {time::MonotonicDuration{100}}) ==
                runtime::CommandRegistrationStatus::registered &&
            dispatcher.freeze_configuration(),
        "command reentrancy setup");

    failures += require(
        dispatcher.dispatch(outer).status ==
                runtime::CommandDispatchStatus::dispatched &&
            handler.nested_result.status ==
                runtime::CommandDispatchStatus::busy,
        "nested command dispatch fails bounded busy");
  }

  {
    DenyPolicy policy;
    runtime::CommandDispatcher dispatcher{
        policy,
        {time::MonotonicDuration{10}}};
    TestHandler handler;
    failures += require(
        dispatcher.register_handler(
            0x3001U,
            handler,
            {time::MonotonicDuration{100}}) ==
                runtime::CommandRegistrationStatus::registered &&
            dispatcher.freeze_configuration() &&
            dispatcher.dispatch(command(20U, 0x3001U)).status ==
                runtime::CommandDispatchStatus::policy_denied &&
            handler.calls == 0U,
        "command policy can fail closed before handler execution");
  }

  {
    runtime::StateRegistry states;
    TestStateProvider provider;
    failures += require(
        states.register_provider(
            provider,
            {time::MonotonicDuration{500}}) ==
                runtime::StateRegistrationStatus::registered,
        "state provider registers");

    std::array<std::byte, 8U> buffer{};
    std::size_t length = 0U;
    runtime::StateHeader header{};
    failures += require(
        states.snapshot(
            provider.state_type(),
            buffer.data(),
            buffer.size(),
            length,
            header) ==
                runtime::StateSnapshotStatus::
                    configuration_not_frozen &&
            states.freeze_configuration(),
        "state registry requires frozen topology");

    failures += require(
        states.snapshot(
            provider.state_type(),
            buffer.data(),
            buffer.size(),
            length,
            header) ==
                runtime::StateSnapshotStatus::ok &&
            length == 3U &&
            buffer[0U] == std::byte{0x11U} &&
            buffer[2U] == std::byte{0x33U} &&
            header.revision == 7U &&
            header.updated_at ==
                time::MonotonicTime{12345} &&
            states.max_snapshot_duration(
                provider.state_type()) ==
                time::MonotonicDuration{500},
        "state snapshot is caller-buffered and revisioned");
  }

  {
    FakeClock clock;
    runtime::EventBus events{clock};
    CountingEventSink exact;
    CountingEventSink wildcard;
    ReentrantEventSink reentrant;
    reentrant.bus = &events;

    failures += require(
        events.subscribe(
            0x2001U,
            exact,
            {time::MonotonicDuration{20}}) ==
                runtime::EventSubscriptionStatus::subscribed &&
            events.subscribe(
                runtime::EventBus::kAllEventTypes,
                wildcard,
                {time::MonotonicDuration{30}}) ==
                runtime::EventSubscriptionStatus::subscribed &&
            events.subscribe(
                0x2001U,
                reentrant,
                {time::MonotonicDuration{40}}) ==
                runtime::EventSubscriptionStatus::subscribed &&
            events.freeze_configuration(),
        "event bus freezes bounded subscriptions");

    const auto published = events.publish({
        0x2001U,
        77U,
        runtime::EventSeverity::info,
        nullptr,
        0U});
    const auto budget = events.max_publish_duration(0x2001U);
    failures += require(
        published.status ==
                runtime::EventPublishStatus::published &&
            published.sequence == 1U &&
            published.deliveries == 3U &&
            exact.calls == 1U &&
            wildcard.calls == 1U &&
            reentrant.calls == 1U &&
            exact.last_sequence == 1U &&
            exact.last_timestamp == clock.now &&
            reentrant.nested.status ==
                runtime::EventPublishStatus::busy &&
            budget.valid &&
            budget.max_duration ==
                time::MonotonicDuration{190},
        "event bus assigns sequence/time and blocks reentrant publish");

    clock.status = time::MonotonicClockStatus::discontinuity;
    failures += require(
        events.publish({
            0x2001U,
            0U,
            runtime::EventSeverity::error,
            nullptr,
            0U}).status ==
                runtime::EventPublishStatus::clock_fault &&
            events.last_sequence() == 1U,
        "event clock fault does not advance sequence");
  }

  {
    runtime::ModuleRegistry modules;
    TestModule first{1U};
    TestModule duplicate_id{1U};
    TestModule second{2U};

    failures += require(
        modules.register_module(first) ==
                runtime::ModuleRegistrationStatus::registered &&
            modules.register_module(first) ==
                runtime::ModuleRegistrationStatus::
                    already_registered &&
            modules.register_module(duplicate_id) ==
                runtime::ModuleRegistrationStatus::id_conflict &&
            modules.register_module(second) ==
                runtime::ModuleRegistrationStatus::registered &&
            modules.freeze_configuration(),
        "module registry enforces stable unique topology");

    failures += require(
        modules.find(1U) == &first &&
            modules.find(2U) == &second &&
            modules.module_count() == 2U &&
            first.start() == runtime::LifecycleStatus::ok &&
            first.service() == runtime::LifecycleStatus::ok &&
            first.stop() == runtime::LifecycleStatus::ok,
        "module registry exposes lifecycle components after freeze");
  }

  {
    runtime::DutRegistry duts;
    const auto egr = actuator_dut(0x100U);
    auto vgt = actuator_dut(0x101U);

    const auto first = duts.register_dut(egr);
    const auto duplicate = duts.register_dut(egr);
    failures += require(
        first.status ==
                runtime::DutRegistrationStatus::registered &&
            first.handle.valid() &&
            duplicate.status ==
                runtime::DutRegistrationStatus::
                    already_registered &&
            duplicate.handle.slot == first.handle.slot &&
            duplicate.handle.generation ==
                first.handle.generation,
        "DUT registry returns stable handle for duplicate descriptor");

    auto conflict = egr;
    conflict.capabilities = domain::dut_capability_mask(
        domain::DutCapability::raw_can);
    failures += require(
        duts.register_dut(conflict).status ==
                runtime::DutRegistrationStatus::
                    profile_id_conflict &&
            duts.register_dut(vgt).status ==
                runtime::DutRegistrationStatus::registered &&
            duts.freeze_configuration(),
        "DUT registry rejects profile-id semantic conflict");

    const auto* found = duts.get(first.handle);
    failures += require(
        found != nullptr &&
            found->profile_id == egr.profile_id &&
            found->dut_class == domain::DutClass::actuator &&
            duts.find_by_profile_id(vgt.profile_id) != nullptr &&
            duts.dut_count() == 2U,
        "DUT registry provides stable frozen descriptors");

    failures += require(
        duts.register_dut(actuator_dut(0x102U)).status ==
                runtime::DutRegistrationStatus::
                    configuration_frozen,
        "DUT topology cannot mutate after freeze");
  }

  if (failures == 0) {
    std::cout << "CORE_V2_RUNTIME_FOUNDATION=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
