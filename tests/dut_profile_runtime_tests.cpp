#include "ecu/dut_profile/runtime.hpp"

#include <cstdint>
#include <iostream>

namespace dp = ecu::dut_profile;
namespace bench = ecu::bench;
namespace monotonic = ecu::core::v2::time;

namespace {

int require(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
  }
  return 0;
}

[[nodiscard]] dp::ResolvedDutSessionPlan make_plan() {
  dp::ResolvedDutSessionPlan plan{};
  plan.schema_version = dp::DutProfileDefinition::kSchemaVersion;
  plan.profile_revision = 7U;
  plan.profile_id = 1234U;
  plan.bench.session_owner = 55U;
  return plan;
}

class FakeProgram final : public dp::IDutProfileProgram {
 public:
  dp::DutProfileProgramDescriptor descriptor_value{
      dp::DutProfileDefinition::kSchemaVersion,
      7U,
      1234U};

  bench::BenchComponentExecutionContract contract{
      monotonic::MonotonicDuration{10},
      monotonic::MonotonicDuration{20},
      monotonic::MonotonicDuration{30},
      monotonic::MonotonicDuration{40},
      monotonic::MonotonicDuration{50}};

  bench::BenchComponentStatus prepare_status{
      bench::BenchComponentStatus::ok};
  bench::BenchComponentStatus activate_status{
      bench::BenchComponentStatus::ok};
  bench::BenchComponentStatus service_status{
      bench::BenchComponentStatus::ok};
  bench::BenchComponentStatus safe_stop_status{
      bench::BenchComponentStatus::ok};
  bench::BenchComponentStatus stop_status{
      bench::BenchComponentStatus::ok};

  std::uint32_t prepare_calls{0U};
  std::uint32_t activate_calls{0U};
  std::uint32_t service_calls{0U};
  std::uint32_t safe_stop_calls{0U};
  std::uint32_t stop_calls{0U};

  const dp::ResolvedDutSessionPlan* first_plan{nullptr};
  const dp::ResolvedDutSessionPlan* last_plan{nullptr};

  [[nodiscard]] dp::DutProfileProgramDescriptor descriptor()
      const noexcept override {
    return descriptor_value;
  }

  [[nodiscard]] bench::BenchComponentExecutionContract
  execution_contract() const noexcept override {
    return contract;
  }

  [[nodiscard]] bench::BenchComponentStatus prepare(
      const dp::ResolvedDutSessionPlan& plan) noexcept override {
    ++prepare_calls;
    if (first_plan == nullptr) {
      first_plan = &plan;
    }
    last_plan = &plan;
    return prepare_status;
  }

  [[nodiscard]] bench::BenchComponentStatus activate(
      const dp::ResolvedDutSessionPlan& plan) noexcept override {
    ++activate_calls;
    last_plan = &plan;
    return activate_status;
  }

  [[nodiscard]] bench::BenchComponentStatus service(
      const dp::ResolvedDutSessionPlan& plan) noexcept override {
    ++service_calls;
    last_plan = &plan;
    return service_status;
  }

  [[nodiscard]] bench::BenchComponentStatus safe_stop(
      const dp::ResolvedDutSessionPlan& plan) noexcept override {
    ++safe_stop_calls;
    last_plan = &plan;
    return safe_stop_status;
  }

  [[nodiscard]] bench::BenchComponentStatus stop(
      const dp::ResolvedDutSessionPlan& plan) noexcept override {
    ++stop_calls;
    last_plan = &plan;
    return stop_status;
  }

  ~FakeProgram() = default;
};

}  // namespace

int main() {
  int failures = 0;

  {
    auto plan = make_plan();
    FakeProgram program;
    dp::DutProfileSessionEndpoint endpoint{plan, program};
    plan.profile_revision = 99U;
    plan.profile_id = 9999U;

    failures += require(endpoint.valid(), "matching program/plan is valid");

    const auto contract = endpoint.execution_contract();
    failures += require(
        contract.max_prepare_duration == monotonic::MonotonicDuration{10} &&
            contract.max_activate_duration == monotonic::MonotonicDuration{20} &&
            contract.max_service_duration == monotonic::MonotonicDuration{30} &&
            contract.max_safe_stop_duration == monotonic::MonotonicDuration{40} &&
            contract.max_stop_duration == monotonic::MonotonicDuration{50},
        "endpoint exposes the bounded program execution contract");

    failures += require(
        endpoint.prepare() == bench::BenchComponentStatus::ok,
        "prepare succeeds");
    failures += require(
        endpoint.prepare() == bench::BenchComponentStatus::no_action &&
            program.prepare_calls == 1U,
        "repeated prepare is idempotent without recalling program");
    failures += require(
        endpoint.activate() == bench::BenchComponentStatus::ok,
        "activate succeeds");

    program.service_status = bench::BenchComponentStatus::no_action;
    failures += require(
        endpoint.service() == bench::BenchComponentStatus::no_action,
        "service no-action remains healthy");

    failures += require(
        endpoint.safe_stop() == bench::BenchComponentStatus::ok,
        "safe stop succeeds");
    failures += require(
        endpoint.safe_stop() == bench::BenchComponentStatus::no_action &&
            program.safe_stop_calls == 1U,
        "repeated safe stop is idempotent");
    failures += require(
        endpoint.stop() == bench::BenchComponentStatus::ok,
        "stop succeeds after safe stop");

    const auto snapshot = endpoint.snapshot();
    failures += require(
        snapshot.valid &&
            snapshot.state == dp::DutProfileEndpointState::idle &&
            snapshot.last_status == bench::BenchComponentStatus::ok &&
            snapshot.profile_id == 1234U &&
            snapshot.profile_revision == 7U &&
            snapshot.counters.prepare_calls == 2U &&
            snapshot.counters.activate_calls == 1U &&
            snapshot.counters.service_calls == 1U &&
            snapshot.counters.safe_stop_calls == 2U &&
            snapshot.counters.stop_calls == 1U &&
            snapshot.counters.faults == 0U,
        "snapshot exposes deterministic lifecycle observability");

    failures += require(
        program.first_plan != nullptr &&
            program.first_plan == program.last_plan &&
            program.last_plan != &plan &&
            program.last_plan->profile_id == 1234U &&
            program.last_plan->profile_revision == 7U,
        "endpoint owns one immutable resolved-plan snapshot");
  }

  {
    auto plan = make_plan();
    FakeProgram program;
    dp::DutProfileSessionEndpoint endpoint{plan, program};

    failures += require(
        endpoint.service() == bench::BenchComponentStatus::fault &&
            program.service_calls == 0U &&
            endpoint.snapshot().state ==
                dp::DutProfileEndpointState::faulted,
        "service before activation fails closed without invoking program");
  }

  {
    auto plan = make_plan();
    FakeProgram program;
    program.service_status = bench::BenchComponentStatus::fault;
    dp::DutProfileSessionEndpoint endpoint{plan, program};

    failures += require(
        endpoint.prepare() == bench::BenchComponentStatus::ok &&
            endpoint.activate() == bench::BenchComponentStatus::ok &&
            endpoint.service() == bench::BenchComponentStatus::fault,
        "program service fault propagates to Bench endpoint");

    failures += require(
        endpoint.snapshot().state == dp::DutProfileEndpointState::faulted &&
            endpoint.snapshot().counters.faults == 1U,
        "runtime latches profile program fault");

    failures += require(
        endpoint.safe_stop() == bench::BenchComponentStatus::ok &&
            program.safe_stop_calls == 1U,
        "faulted endpoint still permits explicit fail-safe stop");
    failures += require(
        endpoint.stop() == bench::BenchComponentStatus::ok &&
            program.stop_calls == 1U &&
            endpoint.snapshot().state == dp::DutProfileEndpointState::idle,
        "fault cleanup can complete after safe stop");
  }

  {
    auto plan = make_plan();
    FakeProgram program;
    program.activate_status = bench::BenchComponentStatus::fault;
    dp::DutProfileSessionEndpoint endpoint{plan, program};

    failures += require(
        endpoint.prepare() == bench::BenchComponentStatus::ok &&
            endpoint.activate() == bench::BenchComponentStatus::fault &&
            endpoint.snapshot().state ==
                dp::DutProfileEndpointState::faulted,
        "activation fault is latched");
    failures += require(
        endpoint.safe_stop() == bench::BenchComponentStatus::ok &&
            endpoint.stop() == bench::BenchComponentStatus::ok,
        "activation failure follows Bench safe-cleanup path");
  }

  {
    auto plan = make_plan();
    FakeProgram program;
    program.descriptor_value.profile_id = 999U;
    dp::DutProfileSessionEndpoint endpoint{plan, program};

    failures += require(
        !endpoint.valid(),
        "profile identity mismatch invalidates runtime endpoint");
    const auto contract = endpoint.execution_contract();
    failures += require(
        contract.max_prepare_duration.count() == 0 &&
            contract.max_service_duration.count() == 0,
        "invalid endpoint advertises no executable budget");
    failures += require(
        endpoint.prepare() == bench::BenchComponentStatus::fault &&
            program.prepare_calls == 0U,
        "invalid endpoint cannot invoke concrete profile program");
  }

  {
    auto plan = make_plan();
    FakeProgram program;
    program.contract.max_service_duration = monotonic::MonotonicDuration{0};
    dp::DutProfileSessionEndpoint endpoint{plan, program};

    failures += require(
        !endpoint.valid() &&
            endpoint.prepare() == bench::BenchComponentStatus::fault &&
            program.prepare_calls == 0U,
        "non-positive program WCET contract is rejected before execution");
  }

  {
    auto plan = make_plan();
    FakeProgram program;
    dp::DutProfileSessionEndpoint endpoint{plan, program};

    failures += require(
        endpoint.prepare() == bench::BenchComponentStatus::ok &&
            endpoint.activate() == bench::BenchComponentStatus::ok,
        "active-stop fixture starts");
    failures += require(
        endpoint.stop() == bench::BenchComponentStatus::fault &&
            program.stop_calls == 0U &&
            endpoint.snapshot().state ==
                dp::DutProfileEndpointState::faulted,
        "direct stop while active cannot bypass safe-stop semantics");
  }

  {
    auto plan = make_plan();
    FakeProgram program;
    dp::DutProfileSessionEndpoint endpoint{plan, program};

    failures += require(
        endpoint.prepare() == bench::BenchComponentStatus::ok &&
            endpoint.safe_stop() == bench::BenchComponentStatus::no_action &&
            program.safe_stop_calls == 0U &&
            endpoint.stop() == bench::BenchComponentStatus::ok,
        "prepared but inactive endpoint needs no active safe-stop frame");
  }

  if (failures == 0) {
    std::cout << "DUT_PROFILE_RUNTIME_CONTRACT_V1=PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
